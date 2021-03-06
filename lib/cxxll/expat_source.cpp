/*
 * Copyright (C) 2013 Red Hat, Inc.
 * Written by Florian Weimer <fweimer@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cxxll/expat_source.hpp>
#include <cxxll/expat_handle.hpp>
#include <cxxll/source.hpp>
#include <cxxll/string_support.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace cxxll;

// We translate the callback-based Expat API into an event-based API.
// We buffer callback events in a vector, using the following
// encoding.  The first byte of every element encodes the type.  We
// use the fact that Expat cannot handle embedded NULs.  This encoding
// is entirely internal.

enum {
  // Start of a start element.  The element name follows as a
  // NUL-terminated string.  Then zero or more ENC_ATTRIBUTE elements,
  // then nothing or a non-ENC_ATTRIBUTE elemement.
  ENC_START = 1,

  // An attribute pair.  Followed by two NUL-terminated strings, key
  // and value.
  ENC_ATTRIBUTE,

  // End of an element.  Nothing follows.
  ENC_END,

  // Followed by the text content as a NUL-terminated string.
  ENC_TEXT,

  // End of stream.
  ENC_EOD
};

struct expat_source::impl {
  expat_handle handle_;
  source *source_;
  size_t consumed_bytes_;
  std::vector<char> upcoming_;
  size_t upcoming_pos_;

  size_t elem_start_;
  size_t elem_len_;		// only covers the name for START
  size_t attr_start_;
  std::string error_;
  state_type state_;
  bool bad_alloc_;

  impl(source *);

  // Is there more data in upcoming_?
  bool remaining() const;

  // Returns the character at upcoming_pos_.  remaining() must be
  // true.
  char tag() const;

  // Returns a strign pointer to the character at upcoming_pos_.
  const char *string_at_pos() const;

  // Returns a pointer one past the end of upcoming_.
  const char *upcoming_end() const;

  // Appends the NUL-terminated string to upcoming_.
  void append_cstr(const char *);

  // Throws illegal_state if the state type does not match state_.
  void check_state(state_type) const;

  // Removes the data before upcoming_pos_ from the upcoming_ vector,
  // and sets upcoming_pos_ to zero.
  void compact();

  // Retrieves more upcoming data via Expat.
  void feed();

  // Throws an exception if an error occured.
  void check_error(enum XML_Status status, const char *, size_t);

  // Expat callback functions.
  static void EntityDeclHandler(void *userData,
				const XML_Char *, int,
				const XML_Char *, int,
				const XML_Char *, const XML_Char *,
				const XML_Char *, const XML_Char *);
  static void StartElementHandler(void *userData,
				  const XML_Char *name,
				  const XML_Char **attrs);
  static void EndElementHandler(void *userData, const XML_Char *);
  static void CharacterDataHandler(void *userData,
				   const XML_Char *s, int len);
};

inline
expat_source::impl::impl(source *src)
  : source_(src), consumed_bytes_(0),
    upcoming_pos_(0), state_(INIT), bad_alloc_(false)
{
  XML_SetUserData(handle_.raw, this);
  XML_SetEntityDeclHandler(handle_.raw, EntityDeclHandler);
  XML_SetElementHandler(handle_.raw, StartElementHandler, EndElementHandler);
  XML_SetCharacterDataHandler(handle_.raw, CharacterDataHandler);
}

inline bool
expat_source::impl::remaining() const
{
  return upcoming_pos_ < upcoming_.size();
}

inline char
expat_source::impl::tag() const
{
  return upcoming_.at(upcoming_pos_);
}

inline const char *
expat_source::impl::string_at_pos() const
{
  return upcoming_.data() + upcoming_pos_;
}

inline const char *
expat_source::impl::upcoming_end() const
{
  return upcoming_.data() + upcoming_.size();
}

void
expat_source::impl::append_cstr(const char *str)
{
  upcoming_.insert(upcoming_.end(), str, str + strlen(str) + 1);
}

void
expat_source::impl::check_state(state_type expected) const
{
  if (state_ != expected) {
    throw illegal_state(state_, expected);
  }
}

void
expat_source::impl::feed()
{
  assert(upcoming_pos_ == upcoming_.size());
  upcoming_.clear();
  upcoming_pos_ = 0;
  char buf[4096];
  do {
    size_t ret = source_->read(reinterpret_cast<unsigned char *>(buf),
			       sizeof(buf));
    check_error(XML_Parse(handle_.raw, buf, ret, /* isFinal */ ret == 0),
		buf, ret);
    consumed_bytes_ += ret;
    if (ret == 0) {
      upcoming_.push_back(ENC_EOD);
    }
  } while(upcoming_.empty());
}

void
expat_source::impl::check_error(enum XML_Status status,
				const char *buf, size_t len)
{
  if (bad_alloc_) {
    throw std::bad_alloc();
  }
  if (!error_.empty()) {
    throw std::runtime_error(error_); // FIXME
  }
  if (status != XML_STATUS_OK) {
    const char *xmlerr = XML_ErrorString(XML_GetErrorCode(handle_.raw));
    unsigned long long line = XML_GetCurrentLineNumber(handle_.raw);
    unsigned long long column = XML_GetCurrentColumnNumber(handle_.raw);
    unsigned long long index = XML_GetCurrentByteIndex(handle_.raw)
      - consumed_bytes_;
    size_t before = std::min(index, 50ULL);
    size_t after = std::min(len - index, 50ULL);
    char *msg;
    int ret = asprintf(&msg, "error=\"%s\" line=%llu column=%llu"
		       " before=\"%s\" after=\"%s\"",
		       quote(xmlerr).c_str(), line, column,
		       quote(std::string(buf + index - before,
					 buf + index)).c_str(),
		       quote(std::string(buf + index,
					 buf + index + after)).c_str());
    if (ret < 0) {
      throw std::bad_alloc();
    }
    try {
      throw std::runtime_error(msg); // FIXME
    } catch (...) {
      free(msg);
      throw;
    }
  }
}

//////////////////////////////////////////////////////////////////////
// Expat callbacks

void
expat_source::impl::EntityDeclHandler(void *userData,
				      const XML_Char *, int,
				      const XML_Char *, int,
				      const XML_Char *, const XML_Char *,
				      const XML_Char *, const XML_Char *)
{
  // Stop the parser when an entity declaration is encountered.
  impl *impl_ = static_cast<impl *>(userData);
  try {
    impl_->error_ = "entity declaration not allowed";
  } catch (std::bad_alloc &) {
    impl_->bad_alloc_ = true;
  }
  XML_StopParser(impl_->handle_.raw, XML_FALSE);
}

void
expat_source::impl::StartElementHandler(void *userData,
					const XML_Char *name,
					const XML_Char **attrs)
{
  impl *impl_ = static_cast<impl *>(userData);
  try {
    impl_->upcoming_.push_back(ENC_START);
    impl_->append_cstr(name);
    while (*attrs) {
      impl_->upcoming_.push_back(ENC_ATTRIBUTE);
      impl_->append_cstr(*attrs);
      ++attrs;
      impl_->append_cstr(*attrs);
      ++attrs;
    }
  } catch (std::bad_alloc &) {
    impl_->bad_alloc_ = true;
    XML_StopParser(impl_->handle_.raw, XML_FALSE);
  }
}

void
expat_source::impl::EndElementHandler(void *userData, const XML_Char *)
{
  impl *impl_ = static_cast<impl *>(userData);
  try {
    impl_->upcoming_.push_back(ENC_END);
  } catch (std::bad_alloc &) {
    impl_->bad_alloc_ = true;
    XML_StopParser(impl_->handle_.raw, XML_FALSE);
  }
}

void
expat_source::impl::CharacterDataHandler(void *userData,
					 const XML_Char *s, int len)
{
  // Expat should error out on embedded NUL characters.
  assert(memchr(s, 0, len) == NULL);
  impl *impl_ = static_cast<impl *>(userData);
  try {
    impl_->upcoming_.push_back(ENC_TEXT);
    impl_->upcoming_.insert(impl_->upcoming_.end(), s, s + len);
    impl_->upcoming_.push_back(0);
  } catch (std::bad_alloc &) {
    impl_->bad_alloc_ = true;
    XML_StopParser(impl_->handle_.raw, XML_FALSE);
  }
}

//////////////////////////////////////////////////////////////////////
// expat_source

expat_source::expat_source(source *src)
  : impl_(new impl(src))
{
}

expat_source::~expat_source()
{
}

bool
expat_source::next()
{
  if (impl_->state_ == EOD) {
    return false;
  }
  if (!impl_->remaining()) {
    impl_->feed();
  }

  // The following decoder assumes that the source is well-formed and
  // trusted.
  switch (impl_->tag()) {
  case ENC_START:
    ++impl_->upcoming_pos_;
    impl_->elem_start_ = impl_->upcoming_pos_;
    impl_->elem_len_ = strlen(impl_->string_at_pos());
    impl_->upcoming_pos_ += impl_->elem_len_ + 1;
    impl_->attr_start_ = impl_->upcoming_pos_;
    while (impl_->remaining() && impl_->tag() == ENC_ATTRIBUTE) {
      // Tag is skipped implicitly along with the key, and the value follows.
      impl_->upcoming_pos_ += strlen(impl_->string_at_pos()) + 1;
      impl_->upcoming_pos_ += strlen(impl_->string_at_pos()) + 1;
    }
    impl_->state_ = START;
    break;
  case ENC_TEXT:
    ++impl_->upcoming_pos_;
    impl_->elem_start_ = impl_->upcoming_pos_;
    impl_->elem_len_ = strlen(impl_->string_at_pos());
    impl_->upcoming_pos_ += impl_->elem_len_ + 1;
    impl_->state_ = TEXT;
    break;
  case ENC_END:
    ++impl_->upcoming_pos_;
    impl_->state_ = END;
    break;
  case ENC_EOD:
    ++impl_->upcoming_pos_;
    impl_->state_ = EOD;
    return false;
  default:
    assert(false);
  }
  return true;
}

expat_source::state_type
expat_source::state() const
{
  return impl_->state_;
}

std::string
expat_source::name() const
{
  impl_->check_state(START);
  const char *p = impl_->upcoming_.data() + impl_->elem_start_;
  return std::string(p, p + impl_->elem_len_);
}

const char *
expat_source::name_ptr() const
{
  impl_->check_state(START);
  return impl_->upcoming_.data() + impl_->elem_start_;
}

std::string
expat_source::attribute(const char *name) const
{
  size_t name_len = strlen(name);
  impl_->check_state(START);
  const char *p = impl_->upcoming_.data() + impl_->attr_start_;
  const char *end = impl_->upcoming_end();
  while (p != end && *p == ENC_ATTRIBUTE) {
    ++p;
    size_t plen = strlen(p);
    if (plen == name_len && memcmp(name, p, plen) == 0) {
      p += plen + 1;
      return std::string(p);
    }
    p += plen + 1; // key
    p += strlen(p) + 1; // value
  }
  return std::string();
}

std::map<std::string, std::string>
expat_source::attributes() const
{
  impl_->check_state(START);
  std::map<std::string, std::string> result;
  attributes(result);
  return result;
}

void
expat_source::attributes(std::map<std::string, std::string> &result) const
{
  impl_->check_state(START);
  const char *p = impl_->upcoming_.data() + impl_->attr_start_;
  const char *end = impl_->upcoming_end();
  while (p != end && *p == ENC_ATTRIBUTE) {
    ++p;
    const char *key = p;
    p += strlen(p) + 1;
    const char *value = p;
    p += strlen(p) + 1;
    result.insert(std::make_pair(std::string(key), std::string(value)));
  }
}

std::string
expat_source::text() const
{
  impl_->check_state(TEXT);
  const char *p = impl_->upcoming_.data() + impl_->elem_start_;
  return std::string(p, impl_->elem_len_);
}

const char *
expat_source::text_ptr() const
{
  impl_->check_state(TEXT);
  return impl_->upcoming_.data() + impl_->elem_start_;
}

std::string
expat_source::text_and_next()
{
  impl_->check_state(TEXT);
  const char *p = impl_->upcoming_.data() + impl_->elem_start_;
  std::string result(p, impl_->elem_len_);
  next();
  while (impl_->state_ == TEXT) {
    p = impl_->upcoming_.data() + impl_->elem_start_;
    result.append(p, impl_->elem_len_);
    next();
  }
  return result;
}

void
expat_source::skip()
{
  switch (impl_->state_) {
  case INIT:
    next();
    break;
  case START:
    {
      unsigned nested = 1;
      next();
      while (nested) {
	switch (impl_->state_) {
	case START:
	  ++nested;
	  break;
	case END:
	  --nested;
	  break;
	case TEXT:
	  break;
	case EOD:
	case INIT:
	  assert(false);
	}
	next();
      }
    }
    break;
  case TEXT:
    do {
      next();
    } while (impl_->state_ == TEXT);
    break;
  case END:
    next();
    break;
  case EOD:
    break;
  }
}

void
expat_source::unnest()
{
  if (impl_->state_ != EOD) {
    while (impl_->state_ != END) {
      skip();
    }
    next();
  }
}

const char *
expat_source::state_string(state_type e)
{
  switch (e) {
  case INIT:
    return "INIT";
  case START:
    return "START";
  case END:
    return "END";
  case TEXT:
    return "TEXT";
  case EOD:
    return "EOD";
  }
  throw std::logic_error("expat_source::state_string");
}


expat_source::illegal_state::illegal_state(state_type actual,
					   state_type expected)
{
  what_ = "actual=";
  what_ += state_string(actual);
  what_ += " expected=";
  what_ += state_string(expected);
}

expat_source::illegal_state::~illegal_state() throw ()
{
}

const char *
expat_source::illegal_state::what() const throw ()
{
  return what_.c_str();
}
