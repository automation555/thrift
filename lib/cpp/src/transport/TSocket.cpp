// Copyright (c) 2006- Facebook
// Distributed under the Thrift Software License
//
// See accompanying file LICENSE or visit the Thrift site at:
// http://developers.facebook.com/thrift/

#include <config.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "concurrency/Monitor.h"
#include "TSocket.h"
#include "TTransportException.h"

namespace facebook { namespace thrift { namespace transport {

using namespace std;

// Global var to track total socket sys calls
uint32_t g_socket_syscalls = 0;

/**
 * TSocket implementation.
 *
 * @author Mark Slee <mcslee@facebook.com>
 */

TSocket::TSocket(string host, int port) :
  host_(host),
  port_(port),
  socket_(-1),
  connTimeout_(0),
  sendTimeout_(0),
  recvTimeout_(0),
  lingerOn_(1),
  lingerVal_(0),
  noDelay_(1),
  maxRecvRetries_(5) {
  recvTimeval_.tv_sec = (int)(recvTimeout_/1000);
  recvTimeval_.tv_usec = (int)((recvTimeout_%1000)*1000);
}

TSocket::TSocket() :
  host_(""),
  port_(0),
  socket_(-1),
  connTimeout_(0),
  sendTimeout_(0),
  recvTimeout_(0),
  lingerOn_(1),
  lingerVal_(0),
  noDelay_(1),
  maxRecvRetries_(5) {
  recvTimeval_.tv_sec = (int)(recvTimeout_/1000);
  recvTimeval_.tv_usec = (int)((recvTimeout_%1000)*1000);
}

TSocket::TSocket(int socket) :
  host_(""),
  port_(0),
  socket_(socket),
  connTimeout_(0),
  sendTimeout_(0),
  recvTimeout_(0),
  lingerOn_(1),
  lingerVal_(0),
  noDelay_(1),
  maxRecvRetries_(5) {
  recvTimeval_.tv_sec = (int)(recvTimeout_/1000);
  recvTimeval_.tv_usec = (int)((recvTimeout_%1000)*1000);
}

TSocket::~TSocket() {
  close();
}

bool TSocket::isOpen() {
  return (socket_ >= 0);
}

bool TSocket::peek() {
  if (!isOpen()) {
    return false;
  }
  uint8_t buf;
  int r = recv(socket_, &buf, 1, MSG_PEEK);
  if (r == -1) {
    int errno_copy = errno;
    string errStr = "TSocket::peek() " + getSocketInfo();
    GlobalOutput(errStr.c_str());
    throw TTransportException(TTransportException::UNKNOWN, "recv()", errno_copy);
  }
  return (r > 0);
}

void TSocket::openConnection(struct addrinfo *res) {
  if (isOpen()) {
    throw TTransportException(TTransportException::ALREADY_OPEN);
  }

  socket_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (socket_ == -1) {
    int errno_copy = errno;
    string errStr = "TSocket::open() socket " + getSocketInfo();
    GlobalOutput(errStr.c_str());
    throw TTransportException(TTransportException::NOT_OPEN, "socket()", errno_copy);
  }

  // Send timeout
  if (sendTimeout_ > 0) {
    setSendTimeout(sendTimeout_);
  }

  // Recv timeout
  if (recvTimeout_ > 0) {
    setRecvTimeout(recvTimeout_);
  }

  // Linger
  setLinger(lingerOn_, lingerVal_);

  // No delay
  setNoDelay(noDelay_);

  // Set the socket to be non blocking for connect if a timeout exists
  int flags = fcntl(socket_, F_GETFL, 0);
  if (connTimeout_ > 0) {
    if (-1 == fcntl(socket_, F_SETFL, flags | O_NONBLOCK)) {
      throw TTransportException(TTransportException::NOT_OPEN, "fcntl() failed");
    }
  } else {
    if (-1 == fcntl(socket_, F_SETFL, flags & ~O_NONBLOCK)) {
      throw TTransportException(TTransportException::NOT_OPEN, "fcntl() failed");
    }
  }

  // Conn timeout
  struct timeval c = {(int)(connTimeout_/1000),
                      (int)((connTimeout_%1000)*1000)};

  // Connect the socket
  int ret = connect(socket_, res->ai_addr, res->ai_addrlen);

  if (ret == 0) {
    goto done;
  }

  if (errno != EINPROGRESS) {
    int errno_copy = errno;
    string errStr = "TSocket::open() connect " + getSocketInfo();
    GlobalOutput(errStr.c_str());
    throw TTransportException(TTransportException::NOT_OPEN, "connect()", errno_copy);
  }

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(socket_, &fds);
  ret = select(socket_+1, NULL, &fds, NULL, &c);

  if (ret > 0) {
    // Ensure connected
    int val;
    socklen_t lon;
    lon = sizeof(int);
    int ret2 = getsockopt(socket_, SOL_SOCKET, SO_ERROR, (void *)&val, &lon);
    if (ret2 == -1) {
      int errno_copy = errno;
      string errStr = "TSocket::open() getsockopt SO_ERROR " + getSocketInfo();
      GlobalOutput(errStr.c_str());
      throw TTransportException(TTransportException::NOT_OPEN, "open()", errno_copy);
    }
    if (val == 0) {
      goto done;
    }
    int errno_copy = errno;
    string errStr = "TSocket::open() SO_ERROR was set " + getSocketInfo();
    GlobalOutput(errStr.c_str());
    throw TTransportException(TTransportException::NOT_OPEN, "open()", errno_copy);
  } else if (ret == 0) {
    int errno_copy = errno;
    string errStr = "TSocket::open() timed out " + getSocketInfo();
    GlobalOutput(errStr.c_str());
    throw TTransportException(TTransportException::NOT_OPEN, "open()", errno_copy);
  } else {
    int errno_copy = errno;
    string errStr = "TSocket::open() select error " + getSocketInfo();
    GlobalOutput(errStr.c_str());
    throw TTransportException(TTransportException::NOT_OPEN, "open()", errno_copy);
  }

 done:
  // Set socket back to normal mode (blocking)
  fcntl(socket_, F_SETFL, flags);
}

void TSocket::open() {
  if (isOpen()) {
    throw TTransportException(TTransportException::ALREADY_OPEN);
  }

  // Validate port number
  if (port_ < 0 || port_ > 65536) {
    throw TTransportException(TTransportException::NOT_OPEN, "Specified port is invalid");
  }

  struct addrinfo hints, *res, *res0;
  int error;
  char port[sizeof("65536") + 1];
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
  sprintf(port, "%d", port_);

  error = getaddrinfo(host_.c_str(), port, &hints, &res0);

  if (error) {
    fprintf(stderr, "getaddrinfo %d: %s\n", error, gai_strerror(error));
    close();
    throw TTransportException(TTransportException::NOT_OPEN, "Could not resolve host for client socket.");
  }

  // Cycle through all the returned addresses until one
  // connects or push the exception up.
  for (res = res0; res; res = res->ai_next) {
    try {
      openConnection(res);
      break;
    } catch (TTransportException& ttx) {
      if (res->ai_next) {
        close();
      } else {
        close();
        freeaddrinfo(res0); // cleanup on failure
        throw;
      }
    }
  }

  // Free address structure memory
  freeaddrinfo(res0);
}

void TSocket::close() {
  if (socket_ >= 0) {
    shutdown(socket_, SHUT_RDWR);
    ::close(socket_);
  }
  socket_ = -1;
}

uint32_t TSocket::read(uint8_t* buf, uint32_t len) {
  if (socket_ < 0) {
    throw TTransportException(TTransportException::NOT_OPEN, "Called read on non-open socket");
  }

  int32_t retries = 0;

  // EAGAIN can be signalled both when a timeout has occurred and when
  // the system is out of resources (an awesome undocumented feature).
  // The following is an approximation of the time interval under which
  // EAGAIN is taken to indicate an out of resources error.
  uint32_t eagainThresholdMicros = 0;
  if (recvTimeout_) {
    // if a readTimeout is specified along with a max number of recv retries, then
    // the threshold will ensure that the read timeout is not exceeded even in the
    // case of resource errors
    eagainThresholdMicros = (recvTimeout_*1000)/ ((maxRecvRetries_>0) ? maxRecvRetries_ : 2);
  }

 try_again:
  // Read from the socket
  struct timeval begin;
  gettimeofday(&begin, NULL);
  int got = recv(socket_, buf, len, 0);
  struct timeval end;
  gettimeofday(&end, NULL);
  uint32_t readElapsedMicros =  (((end.tv_sec - begin.tv_sec) * 1000 * 1000)
                                 + (((uint64_t)(end.tv_usec - begin.tv_usec))));
  ++g_socket_syscalls;

  // Check for error on read
  if (got < 0) {
    if (errno == EAGAIN) {
      // check if this is the lack of resources or timeout case
      if (!eagainThresholdMicros || (readElapsedMicros < eagainThresholdMicros)) {
        if (retries++ < maxRecvRetries_) {
          usleep(50);
          goto try_again;
        } else {
          throw TTransportException(TTransportException::TIMED_OUT,
                                    "EAGAIN (unavailable resources)");
        }
      } else {
        // infer that timeout has been hit
        throw TTransportException(TTransportException::TIMED_OUT,
                                  "EAGAIN (timed out)");
      }
    }

    // If interrupted, try again
    if (errno == EINTR && retries++ < maxRecvRetries_) {
      goto try_again;
    }

    // Now it's not a try again case, but a real probblez
    string errStr = "TSocket::read() " + getSocketInfo();
    GlobalOutput(errStr.c_str());

    // If we disconnect with no linger time
    if (errno == ECONNRESET) {
      throw TTransportException(TTransportException::NOT_OPEN, "ECONNRESET");
    }

    // This ish isn't open
    if (errno == ENOTCONN) {
      throw TTransportException(TTransportException::NOT_OPEN, "ENOTCONN");
    }

    // Timed out!
    if (errno == ETIMEDOUT) {
      throw TTransportException(TTransportException::TIMED_OUT, "ETIMEDOUT");
    }

    // Some other error, whatevz
    char buff[1024];
    sprintf(buff, "ERROR errno: %d", errno);
    throw TTransportException(TTransportException::UNKNOWN, buff);
  }

  // The remote host has closed the socket
  if (got == 0) {
    close();
    return 0;
  }

  // Pack data into string
  return got;
}

void TSocket::write(const uint8_t* buf, uint32_t len) {
  if (socket_ < 0) {
    throw TTransportException(TTransportException::NOT_OPEN, "Called write on non-open socket");
  }

  uint32_t sent = 0;

  while (sent < len) {

    int flags = 0;
    #ifdef MSG_NOSIGNAL
    // Note the use of MSG_NOSIGNAL to suppress SIGPIPE errors, instead we
    // check for the EPIPE return condition and close the socket in that case
    flags |= MSG_NOSIGNAL;
    #endif // ifdef MSG_NOSIGNAL

    int b = send(socket_, buf + sent, len - sent, flags);
    ++g_socket_syscalls;

    // Fail on a send error
    if (b < 0) {
      if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
        int errno_copy = errno;
        close();
        throw TTransportException(TTransportException::NOT_OPEN, "send()", errno_copy);
      }

      int errno_copy = errno;
      string errStr = "TSocket::write() send < 0 " + getSocketInfo();
      GlobalOutput(errStr.c_str());
      throw TTransportException(TTransportException::UNKNOWN, "send", errno_copy);
    }

    // Fail on blocked send
    if (b == 0) {
      throw TTransportException(TTransportException::NOT_OPEN, "Socket send returned 0.");
    }
    sent += b;
  }
}

std::string TSocket::getHost() {
  return host_;
}

int TSocket::getPort() {
  return port_;
}

void TSocket::setHost(string host) {
  host_ = host;
}

void TSocket::setPort(int port) {
  port_ = port;
}

void TSocket::setLinger(bool on, int linger) {
  lingerOn_ = on;
  lingerVal_ = linger;
  if (socket_ < 0) {
    return;
  }

  struct linger l = {(lingerOn_ ? 1 : 0), lingerVal_};
  int ret = setsockopt(socket_, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
  if (ret == -1) {
    string errStr = "TSocket::setLinger() " + getSocketInfo();
    GlobalOutput(errStr.c_str());
  }
}

void TSocket::setNoDelay(bool noDelay) {
  noDelay_ = noDelay;
  if (socket_ < 0) {
    return;
  }

  // Set socket to NODELAY
  int v = noDelay_ ? 1 : 0;
  int ret = setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
  if (ret == -1) {
    string errStr = "TSocket::setNoDelay() " + getSocketInfo();
    GlobalOutput(errStr.c_str());
  }
}

void TSocket::setConnTimeout(int ms) {
  connTimeout_ = ms;
}

void TSocket::setRecvTimeout(int ms) {
  if (ms < 0) {
    char errBuf[512];
    sprintf(errBuf, "TSocket::setRecvTimeout with negative input: %d\n", ms);
    GlobalOutput(errBuf);
    return;
  }
  recvTimeout_ = ms;

  if (socket_ < 0) {
    return;
  }

  recvTimeval_.tv_sec = (int)(recvTimeout_/1000);
  recvTimeval_.tv_usec = (int)((recvTimeout_%1000)*1000);

  // Copy because select may modify
  struct timeval r = recvTimeval_;
  int ret = setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r));
  if (ret == -1) {
    string errStr = "TSocket::setRecvTimeout() " + getSocketInfo();
    GlobalOutput(errStr.c_str());
  }
}

void TSocket::setSendTimeout(int ms) {
  if (ms < 0) {
    char errBuf[512];
    sprintf(errBuf, "TSocket::setSendTimeout with negative input: %d\n", ms);
    GlobalOutput(errBuf);
    return;
  }
  sendTimeout_ = ms;

  if (socket_ < 0) {
    return;
  }

  struct timeval s = {(int)(sendTimeout_/1000),
                      (int)((sendTimeout_%1000)*1000)};
  int ret = setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof(s));
  if (ret == -1) {
    string errStr = "TSocket::setSendTimeout() " + getSocketInfo();
    GlobalOutput(errStr.c_str());
  }
}

void TSocket::setMaxRecvRetries(int maxRecvRetries) {
  maxRecvRetries_ = maxRecvRetries;
}

string TSocket::getSocketInfo() {
  std::ostringstream oss;
  oss << "<Host: " << host_ << " Port: " << port_ << ">";
  return oss.str();
}

std::string TSocket::getPeerHost() {
  if (peerHost_.empty()) {
    struct sockaddr_storage addr;
    socklen_t addrLen = sizeof(addr);

    if (socket_ < 0) {
      return host_;
    }

    int rv = getpeername(socket_, (sockaddr*) &addr, &addrLen);

    if (rv != 0) {
      return peerHost_;
    }

    char clienthost[NI_MAXHOST];
    char clientservice[NI_MAXSERV];

    getnameinfo((sockaddr*) &addr, addrLen,
                clienthost, sizeof(clienthost),
                clientservice, sizeof(clientservice), 0);

    peerHost_ = clienthost;
  }
  return peerHost_;
}

std::string TSocket::getPeerAddress() {
  if (peerAddress_.empty()) {
    struct sockaddr_storage addr;
    socklen_t addrLen = sizeof(addr);

    if (socket_ < 0) {
      return peerAddress_;
    }

    int rv = getpeername(socket_, (sockaddr*) &addr, &addrLen);

    if (rv != 0) {
      return peerAddress_;
    }

    char clienthost[NI_MAXHOST];
    char clientservice[NI_MAXSERV];

    getnameinfo((sockaddr*) &addr, addrLen,
                clienthost, sizeof(clienthost),
                clientservice, sizeof(clientservice),
                NI_NUMERICHOST|NI_NUMERICSERV);

    peerAddress_ = clienthost;
    peerPort_ = std::atoi(clientservice);
  }
  return peerAddress_;
}

int TSocket::getPeerPort() {
  getPeerAddress();
  return peerPort_;
}

}}} // facebook::thrift::transport
