/*

Copyright (C) 2010-2013 KWARC Group <kwarc.info>

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
  * @brief  Content MathML Token implementation
  * @file   CmmlToken.cpp
  * @author Corneliu-Claudiu Prodescu <c.prodescu@jacobs-university.de>
  * @date   12 Oct 2010
  *
  * License: GPL v3
  *
  */

#include <assert.h>
#include <ctype.h>
#include <string.h>

#include <map>
using std::map;
#include <sstream>
using std::stringstream;
#include <string>
using std::string;

#include "common/utils/ToString.hpp"

#include "CmmlToken.hpp"


namespace mws {
namespace types {


const Meaning MWS_QVAR_MEANING = "mws:qvar";
const char ROOT_XPATH_SELECTOR[] = "/*[1]";

CmmlToken::CmmlToken(bool aMode) :
    _tag(""),
    _textContent(""),
    _parentNode(NULL),
    _xpath(ROOT_XPATH_SELECTOR),
    _mode(aMode) {
}


CmmlToken*
CmmlToken::newRoot(bool aMode) {
    return (new CmmlToken(aMode));
}


CmmlToken::~CmmlToken() {
    while (!_childNodes.empty()) {
        delete _childNodes.front();
        _childNodes.pop_front();
    }
}


void
CmmlToken::setTag(const std::string& aTag) {
    if (aTag.compare(0, 2, "m:") == 0) {
        _tag = aTag.substr(2, aTag.size() - 2);
    } else {
        _tag            = aTag;
    }
}


void
CmmlToken::addAttribute(const std::string& anAttribute,
                        const std::string& aValue) {
    _attributes.insert(make_pair(anAttribute, aValue));
}

std::string
CmmlToken::getAttribute(const std::string& anAttribute) const{
    std::map<std::string, std::string>::const_iterator it = _attributes.find(anAttribute);
    if (it != _attributes.end()) {
        return it->second;
    }
    return "";
}

void
CmmlToken::appendTextContent(const char* aTextContent,
                             size_t      nBytes) {
    _textContent.reserve(_textContent.size() + nBytes);
    for (size_t i = 0; i < nBytes; i++) {
        if (!isspace(aTextContent[i])) {
            _textContent.append(1, aTextContent[i]);
        }
    }
}


const string&
CmmlToken::getTextContent() const {
    return _textContent;
}


CmmlToken*
CmmlToken::newChildNode() {
    CmmlToken* result;

    result = new CmmlToken(_mode);
    _childNodes.push_back(result);
    result->_parentNode = this;
    result->_xpath = _xpath + "/*[" + ToString(_childNodes.size()) + "]";

    return result;
}


bool
CmmlToken::isRoot() const {
    return (_parentNode == NULL);
}


bool
CmmlToken::isVar() const {
    return (getType() == VAR);
}

CmmlToken*
CmmlToken::getParentNode() const {
    return _parentNode;
}


const CmmlToken::PtrList&
CmmlToken::getChildNodes() const {
    return _childNodes;
}


const string&
CmmlToken::getXpath() const {
    return _xpath;
}


string
CmmlToken::getXpathRelative() const {
    // xpath without initial /*[1]
    string xpath_relative(_xpath, strlen(ROOT_XPATH_SELECTOR), string::npos);

    return xpath_relative;
}

string
CmmlToken::toString(int indent) const {
    stringstream ss;
    string       padding;
    map<string, string> :: const_iterator mIt;
    PtrList :: const_iterator lIt;

    padding.append(indent, ' ');

    ss << padding << "<" << _tag << " ";

    for (mIt = _attributes.begin(); mIt != _attributes.end(); mIt ++) {
        ss << mIt->first << "=\"" << mIt->second << "\" ";
    }

    ss << ">" << _textContent;

    if (_childNodes.size()) {
        ss << "\n";

        for (lIt = _childNodes.begin(); lIt != _childNodes.end(); lIt ++) {
            ss << (*lIt)->toString(indent + 2);
        }

        ss << padding;
    }

    ss << "</" << _tag << ">\n";

    return ss.str();
}

uint32_t
CmmlToken::getExprDepth() const {
    uint32_t max_depth = 0;
    for (auto child : _childNodes) {
        uint32_t depth = child->getExprDepth() + 1;
        if (depth > max_depth) max_depth = depth;
    }

    return max_depth;
}

uint32_t
CmmlToken::getExprSize() const {
    uint32_t size = 1;  // counting current token

    for (auto child : _childNodes) {
        size += child->getExprSize();
    }

    return size;
}

CmmlToken::Type
CmmlToken::getType() const {
    if (_tag == MWS_QVAR_MEANING) {
        return VAR;
    } else {
        return CONSTANT;
    }
}

const std::string&
CmmlToken::getVarName() const {
    assert(getType() == VAR);

    return _textContent;
}

std::string
CmmlToken::getMeaning() const {
    // assert(getType() == CONSTANT); XXX legacy mwsd still uses this

    string meaning;
    if (_tag == MWS_QVAR_MEANING) {
        meaning = MWS_QVAR_MEANING;
    } else if (_tag == "apply" || _textContent.empty()) {
        meaning = _tag;
    } else {
        // to avoid ambiguity between <ci>eq</ci> and <m:eq/>
        meaning = "#" + _textContent;
    }

    return meaning;
}

uint32_t
CmmlToken::getArity() const {
    return _childNodes.size();
}

bool
CmmlToken::equals(const CmmlToken *t) const {
    if (this->getType() != t->getType()) return false;
    if (this->getMeaning() != t->getMeaning()) return false;
    if (_childNodes.size() != t->_childNodes.size()) return false;

    PtrList::const_iterator it1, it2;
    it1 = _childNodes.begin();
    it2 = t->_childNodes.begin();
    while (it1 != _childNodes.end()) {
        if (!(*it1)->equals(*it2)) return false;

        it1++;
        it2++;
    }

    return true;
}

}  // namespace types
}  // namespace mws
