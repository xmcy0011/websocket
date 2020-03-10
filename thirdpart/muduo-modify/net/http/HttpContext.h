// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_HTTP_HTTPCONTEXT_H
#define MUDUO_NET_HTTP_HTTPCONTEXT_H

#include "muduo/base/copyable.h"

#include "muduo/net/http/HttpRequest.h"

#include "libwebsocket/IHttpContext.h"

namespace muduo
{
namespace net
{

class Buffer;

class HttpContext : public muduo::copyable, public http::IContext
{
 public:
//   enum HttpRequestParseState
//   {
//     kExpectRequestLine,
//     kExpectHeaders,
//     kExpectBody,
//     kGotAll,
//   };
     typedef http::IContext::ParseState HttpRequestParseState;
  HttpContext()
    : state_(http::IContext::kExpectRequestLine)
  {
  }

  // default copy-ctor, dtor and assignment are fine

  // return false if any error
  bool parseRequest(Buffer* buf, Timestamp receiveTime);

  bool parseRequestPtr(IBytesBuffer* buf, ITimestamp* receiveTime) {
      assert(buf);
      assert(receiveTime);
      Buffer* buff = reinterpret_cast<Buffer*>(buf);
      assert(buff);
      return parseRequest(buff, Timestamp(receiveTime->microSecondsSinceEpoch()));
  }

  bool gotAll() const
  { return state_ == http::IContext::kGotAll; }

  void reset()
  {
    state_ = http::IContext::kExpectRequestLine;
    HttpRequest dummy;
    request_.swap(dummy);
  }

  http::IRequest const* requestConstPtr() const {
      return &request_;
  }

  http::IRequest* requestPtr() {
      return &request_;
  }
  
  const HttpRequest& request() const
  { return request_; }

  HttpRequest& request()
  { return request_; }

 private:
  bool processRequestLine(const char* begin, const char* end);

  http::IContext::ParseState state_;
  HttpRequest request_;
};

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_HTTP_HTTPCONTEXT_H
