// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/io_buffer.h"

#include "base/logging.h"

namespace net {

IOBuffer::IOBuffer()
    : data_(NULL) {
}

IOBuffer::IOBuffer(int buffer_size) {
  DCHECK(buffer_size > 0);
  data_ = new char[buffer_size];
}

IOBuffer::IOBuffer(char* data)
    : data_(data) {
}

IOBuffer::~IOBuffer() {
  delete[] data_;
}

IOBufferWithSize::IOBufferWithSize(int size)
    : IOBuffer(size),
      size_(size) {
}

StringIOBuffer::StringIOBuffer(const std::string& s)
    : IOBuffer(static_cast<char*>(NULL)),
      string_data_(s) {
  data_ = const_cast<char*>(string_data_.data());
}

StringIOBuffer::~StringIOBuffer() {
  // We haven't allocated the buffer, so remove it before the base class
  // destructor tries to delete[] it.
  data_ = NULL;
}

DrainableIOBuffer::DrainableIOBuffer(IOBuffer* base, int size)
    : IOBuffer(base->data()),
      base_(base),
      size_(size),
      used_(0) {
}

DrainableIOBuffer::~DrainableIOBuffer() {
  // The buffer is owned by the |base_| instance.
  data_ = NULL;
}

void DrainableIOBuffer::DidConsume(int bytes) {
  SetOffset(used_ + bytes);
}

int DrainableIOBuffer::BytesRemaining() const {
  return size_ - used_;
}

// Returns the number of consumed bytes.
int DrainableIOBuffer::BytesConsumed() const {
  return used_;
}

void DrainableIOBuffer::SetOffset(int bytes) {
  DCHECK(bytes >= 0 && bytes <= size_);
  used_ = bytes;
  data_ = base_->data() + used_;
}

GrowableIOBuffer::GrowableIOBuffer()
    : IOBuffer(),
      capacity_(0),
      offset_(0) {
}

GrowableIOBuffer::~GrowableIOBuffer() {
  data_ = NULL;
}

void GrowableIOBuffer::SetCapacity(int capacity) {
  DCHECK(capacity >= 0);
  // realloc will crash if it fails.
  real_data_.reset(static_cast<char*>(realloc(real_data_.release(), capacity)));
  capacity_ = capacity;
  if (offset_ > capacity)
    set_offset(capacity);
  else
    set_offset(offset_);  // The pointer may have changed.
}

void GrowableIOBuffer::set_offset(int offset) {
  DCHECK(offset >= 0 && offset <= capacity_);
  offset_ = offset;
  data_ = real_data_.get() + offset;
}

int GrowableIOBuffer::RemainingCapacity() {
  return capacity_ - offset_;
}

char* GrowableIOBuffer::StartOfBuffer() {
  return real_data_.get();
}

PickledIOBuffer::PickledIOBuffer() : IOBuffer() {}

PickledIOBuffer::~PickledIOBuffer() { data_ = NULL; }

void PickledIOBuffer::Done() {
  data_ = const_cast<char*>(static_cast<const char*>(pickle_.data()));
}


WrappedIOBuffer::WrappedIOBuffer(const char* data)
    : IOBuffer(const_cast<char*>(data)) {
}

WrappedIOBuffer::~WrappedIOBuffer() {
  data_ = NULL;
}

}  // namespace net
