/*Copyright (C) 2010-2013 KWARC Group <kwarc.info>

This file is part of MathWebSearch.

MathWebSearch is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

MathWebSearch is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with MathWebSearch.  If not, see <http://www.gnu.org/licenses/>.

*/
/**
  * @brief File containing the implementation of the Daemon class.
  * @file Daemon.cpp
  * @author Radu Hambasan
  * @author Corneliu Prodescu
  * @date 20 Mar 2014
  *
  * License: GPL v3
  */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <memory>
using std::unique_ptr;
#include <stdexcept>
using std::runtime_error;

#include "common/utils/compiler_defs.h"
#include "common/utils/memstream.h"
#include "common/utils/util.hpp"
using common::utils::formattedString;
#include "mws/types/Query.hpp"
using mws::types::Query;
#include "mws/xmlparser/readMwsQuery.hpp"
using mws::xmlparser::readMwsQuery;
using mws::xmlparser::QueryMode;
#include "mws/daemon/SchemaQueryHandler.hpp"
using mws::daemon::SchemaQueryHandler;
#include "mws/daemon/GenericHttpResponses.hpp"
#include "mws/daemon/microhttpd_linux.h"
#include "mws/daemon/Daemon.hpp"

#include "build-gen/config.h"

namespace mws {
namespace daemon {

Daemon::Config::Config() : port(DEFAULT_MWS_PORT), enableIpv6(false) {}

class MemStream {
    enum Mode {
        MODE_WRITING,
        MODE_READING_BUFFER,
        MODE_READING_FILE
    } _mode;
    char* _buffer;
    size_t _buffer_size;
    FILE* _input;
    FILE* _output;

 public:
    struct Buffer {
        char* data;
        size_t size;

        Buffer(char* data, size_t size) : data(data), size(size) {}
    };

    MemStream()
        : _mode(MODE_WRITING),
          _buffer(nullptr),
          _buffer_size(0),
          _output(nullptr) {
        _input = open_memstream(&_buffer, &_buffer_size);
        assert(_input != nullptr);
    }

    ~MemStream() {
        switch (_mode) {
        case MODE_WRITING:
            closeWritingMode();
            break;
        case MODE_READING_FILE:
            closeReadingFileMode();
            break;
        case MODE_READING_BUFFER:
            break;
        default:
            assert(false);
            break;
        }
        assert(_input == nullptr);
        assert(_output == nullptr);
        free(_buffer);
    }

    FILE* getInput() { return _input; }

    FILE* getOutputFile() {
        closeWritingMode();
        _mode = MODE_READING_FILE;
        _output = fmemopen(_buffer, _buffer_size, "r");
        // fmemopen shouldn't fail if _buffer_size > 0
        if (_buffer_size != 0) assert(_output != nullptr);
        return _output;
    }

    Buffer getOutputBuffer() {
        closeWritingMode();
        _mode = MODE_READING_BUFFER;
        return Buffer(_buffer, _buffer_size);
    }

    Buffer releaseOutputBuffer() {
        Buffer buffer = getOutputBuffer();
        _buffer = nullptr;
        return buffer;
    }

 private:
    void closeWritingMode() {
        assert(_mode == MODE_WRITING);
        if (_input != nullptr) fclose(_input);
        _input = nullptr;
    }

    void closeReadingFileMode() {
        assert(_mode == MODE_READING_FILE);
        if (_output != nullptr) fclose(_output);
        _output = nullptr;
    }
};

static int acceptPolicyCallback(void* cls, const sockaddr* addr,
                                socklen_t addrlen) {
    UNUSED(cls);
    UNUSED(addr);
    UNUSED(addrlen);

    // Accepting everything
    return MHD_YES;
}



static int accessHandlerCallback(void* cls, struct MHD_Connection* connection,
                                 const char* url, const char* method,
                                 const char* version, const char* upload_data,
                                 size_t* upload_data_size, void** ptr) {
    UNUSED(version);

    // check if we have the root url
    bool isRootURL = strcmp(url, ROOT_URL) == 0;

    // On OPTIONS method request different behavior
    if (0 == strcmp(method, MHD_HTTP_METHOD_OPTIONS)) {
        return sendOptionsResponse(connection, isRootURL);
    }

    // If we have a GET request, and we have the ROOT url
    // we should send
    if ((0 == strcmp(method, MHD_HTTP_METHOD_GET)) && isRootURL) {
        return sendXmlGenericResponse(connection, XML_MWS_ROOT_RESPONSE, MHD_HTTP_OK);
    }

    // Accept only POST requests
    if (0 != strcmp(method, MHD_HTTP_METHOD_POST)) {
        return sendMethodNotAllowedResponse(connection, isRootURL);
    }

    // Allocate handler data
    if (*ptr == nullptr) {
        *ptr = new MemStream();
        return MHD_YES;
    }
    MemStream* memstream = (MemStream*)*ptr;

    // Process data
    if (*upload_data_size) {
        while (*upload_data_size) {
            size_t nbytes = fwrite(upload_data, sizeof(char), *upload_data_size,
                                   memstream->getInput());
            if (nbytes <= 0) {
                delete memstream;
                return MHD_NO;
            }

            upload_data += nbytes;
            *upload_data_size -= nbytes;
        }
        return MHD_YES;
    }

    QueryHandler* qh = reinterpret_cast<QueryHandler*>(cls);
    // Parse query
    QueryMode qMode = QueryMode::QUERY_MWS;
    // is instance of SchemaQueryHandler
    if (dynamic_cast<SchemaQueryHandler*>(qh) != nullptr) {
        qMode = QueryMode::QUERY_SCHEMA;
    }
    unique_ptr<Query> mwsQuery(readMwsQuery(memstream->getOutputFile(), qMode));
    delete memstream;

    // Check if query failed or is empty
    if (mwsQuery == nullptr || mwsQuery->tokens.size() == 0) {
        PRINT_WARN("Bad query request\n");
        return sendXmlGenericResponse(connection, XML_MWS_BAD_QUERY,
                                      MHD_HTTP_BAD_REQUEST);
    }

// Process query
#ifdef APPLY_RESTRICTIONS
    mwsQuery->applyRestrictions();
#endif

    unique_ptr<GenericAnswer> answset(qh->handleQuery(mwsQuery.get()));
    if (answset == nullptr) {
        PRINT_WARN("Error while obtaining answer set\n");
        return sendXmlGenericResponse(connection, XML_MWS_SERVER_ERROR,
                                      MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    // Write answer
    MemStream responseData;
    int ret = mwsQuery->responseFormatter
        ->writeData(answset.get(), responseData.getInput());

    if (ret < 0) {
        PRINT_WARN("Error while writing the Answer Set\n");
        return sendXmlGenericResponse(connection, XML_MWS_SERVER_ERROR,
                                      MHD_HTTP_INTERNAL_SERVER_ERROR);
    } else {
        PRINT_LOG("Response of %d bytes sent.\n", ret);
    }

    // Compose and send response
    MemStream::Buffer responseDataBuffer = responseData.releaseOutputBuffer();
    struct MHD_Response* response;
#ifndef MICROHTTPD_DEPRECATED
    response = MHD_create_response_from_buffer(responseDataBuffer.size,
                                               responseDataBuffer.data,
                                               MHD_RESPMEM_MUST_FREE);
#else
    response = MHD_create_response_from_data(responseDataBuffer.size,
                                             responseDataBuffer.data,
                                             /* must_free = */ 1,
                                             /* must_copy = */ 0);
#endif
    MHD_add_response_header(response, "Content-Type",
                            mwsQuery->responseFormatter->getContentType());
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Cache-Control",
                            "no-cache, must-revalidate");
    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    return ret;
}

Daemon::Daemon(QueryHandler* queryHandler, const Config& config)
    : _queryHandler(queryHandler) {

    if (queryHandler == nullptr) {
        throw runtime_error("queryHandler is NULL\n");
    }

    unsigned int mhd_flags = 0;
    mhd_flags |= MHD_USE_THREAD_PER_CONNECTION;
    if (config.enableIpv6) {
        mhd_flags |= MHD_USE_IPv6;
    }
    _mhd = MHD_start_daemon(mhd_flags, config.port, acceptPolicyCallback,
                            /* PolicyContext = */ nullptr,
                            accessHandlerCallback, _queryHandler.get(),
                            MHD_OPTION_CONNECTION_LIMIT, 20, MHD_OPTION_END);
    if (_mhd == nullptr) {
        throw runtime_error(
            formattedString("MHD_start_daemon: %s", strerror(errno)));
    }

    PRINT_LOG("Listening on port %d\n", config.port);
}

Daemon::~Daemon() {
    if (_mhd != nullptr) {
        MHD_stop_daemon(_mhd);
    }
}

}  // namespace daemon
}  // namespace mws
