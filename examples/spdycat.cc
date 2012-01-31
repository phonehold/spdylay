/*
 * Spdylay - SPDY Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <getopt.h>

#include <cassert>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iostream>
#include <string>
#include <set>
#include <iomanip>
#include <fstream>
#include <map>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <spdylay/spdylay.h>

#include "spdylay_ssl.h"
#include "uri.h"

namespace spdylay {

struct Config {
  bool null_out;
  bool remote_name;
  bool verbose;
  Config():null_out(false), remote_name(false), verbose(false) {}
};

struct Request {
  uri::UriStruct us;
  Request(const uri::UriStruct& us):us(us) {}
};

std::map<int32_t, Request*> stream2req;
std::map<std::string, Request*> path2req;
size_t numreq, complete;

Config config;
extern bool ssl_debug;

void on_data_chunk_recv_callback
(spdylay_session *session, uint8_t flags, int32_t stream_id,
 const uint8_t *data, size_t len, void *user_data)
{
  std::map<int32_t, Request*>::iterator itr = stream2req.find(stream_id);
  if(itr != stream2req.end()) {
    std::cout.write(reinterpret_cast<const char*>(data), len);
  }
}

void check_stream_id(spdylay_frame_type type, spdylay_frame *frame)
{
  if(type == SPDYLAY_SYN_STREAM) {
    for(int i = 0; frame->syn_stream.nv[i]; i += 2) {
      if(strcmp("url", frame->syn_stream.nv[i]) == 0) {
        std::map<std::string, Request*>::iterator itr = path2req.find
          (frame->syn_stream.nv[i+1]);
        if(itr != path2req.end()) {
          stream2req[frame->syn_stream.stream_id] = (*itr).second;
        }
        break;
      }
    }
  }
}

void on_ctrl_send_callback2
(spdylay_session *session, spdylay_frame_type type, spdylay_frame *frame,
 void *user_data)
{
  check_stream_id(type, frame);
}

void on_ctrl_send_callback3
(spdylay_session *session, spdylay_frame_type type, spdylay_frame *frame,
 void *user_data)
{
  check_stream_id(type, frame);
  on_ctrl_send_callback(session, type, frame, user_data);
}

void on_stream_close_callback
(spdylay_session *session, int32_t stream_id, spdylay_status_code status_code,
 void *user_data)
{
  std::map<int32_t, Request*>::iterator itr = stream2req.find(stream_id);
  if(itr != stream2req.end()) {
    ++complete;
    if(complete == numreq) {
      spdylay_submit_goaway(session);
    }
  }
}

int communicate(const std::string& host, uint16_t port,
                std::vector<Request>& reqvec,
                const spdylay_session_callbacks *callbacks)
{
  numreq = reqvec.size();
  complete = 0;
  path2req.clear();
  stream2req.clear();
  int fd = connect_to(host, port);
  if(fd == -1) {
    std::cerr << "Could not connect to the host" << std::endl;
    return -1;
  }
  SSL_CTX *ssl_ctx;
  ssl_ctx = SSL_CTX_new(SSLv23_client_method());
  if(!ssl_ctx) {
    std::cerr << ERR_error_string(ERR_get_error(), 0) << std::endl;
    return -1;
  }
  setup_ssl_ctx(ssl_ctx);
  SSL *ssl = SSL_new(ssl_ctx);
  if(!ssl) {
    std::cerr << ERR_error_string(ERR_get_error(), 0) << std::endl;
    return -1;
  }
  if(ssl_handshake(ssl, fd) == -1) {
    return -1;
  }
  make_non_block(fd);
  Spdylay sc(fd, ssl, callbacks);
  int epollfd = epoll_create(1);
  if(epollfd == -1) {
    perror("epoll_create");
    return -1;
  }
  for(int i = 0, n = reqvec.size(); i < n; ++i) {
    uri::UriStruct& us = reqvec[i].us;
    std::string path = us.dir+us.file+us.query;
    int r = sc.submit_request(path, 3);
    assert(r == 0);
    path2req[path] = &reqvec[i];
  }
  ctl_epollev(epollfd, EPOLL_CTL_ADD, &sc);
  static const size_t MAX_EVENTS = 1;
  epoll_event events[MAX_EVENTS];
  bool ok = true;
  while(sc.want_read() || sc.want_write()) {
    int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if(nfds == -1) {
      perror("epoll_wait");
      return -1;
    }
    for(int n = 0; n < nfds; ++n) {
      if(((events[n].events & EPOLLIN) && sc.recv() != 0) ||
         ((events[n].events & EPOLLOUT) && sc.send() != 0)) {
        ok = false;
        std::cout << "Fatal" << std::endl;
        break;
      }
      if((events[n].events & EPOLLHUP) || (events[n].events & EPOLLERR)) {
        std::cout << "HUP" << std::endl;
        ok = false;
        break;
      }
    }
    if(!ok) {
      break;
    }
    ctl_epollev(epollfd, EPOLL_CTL_MOD, &sc);
  }

  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ssl_ctx);
  shutdown(fd, SHUT_WR);
  close(fd);
  return ok ? 0 : -1;
}

int run(char **uris, int n)
{
  spdylay_session_callbacks callbacks;
  memset(&callbacks, 0, sizeof(spdylay_session_callbacks));
  callbacks.send_callback = send_callback;
  callbacks.recv_callback = recv_callback;
  callbacks.on_stream_close_callback = on_stream_close_callback;
  if(config.verbose) {
    callbacks.on_ctrl_recv_callback = on_ctrl_recv_callback;
    callbacks.on_data_recv_callback = on_data_recv_callback;
    callbacks.on_ctrl_send_callback = on_ctrl_send_callback3;
  } else {
    callbacks.on_ctrl_send_callback = on_ctrl_send_callback2;
  }
  if(!config.null_out) {
    callbacks.on_data_chunk_recv_callback = on_data_chunk_recv_callback;
  }
  ssl_debug = config.verbose;
  std::vector<Request> reqvec;
  std::string prev_host;
  uint16_t prev_port = 0;
  for(int i = 0; i < n; ++i) {
    uri::UriStruct us;
    if(uri::parse(us, uris[i])) {
      if(prev_host != us.host || prev_port != us.port) {
        if(!reqvec.empty()) {
          communicate(prev_host, prev_port, reqvec, &callbacks);
          reqvec.clear();
        }
        prev_host = us.host;
        prev_port = us.port;
      }
      reqvec.push_back(Request(us));
    }
  }
  if(!reqvec.empty()) {
    communicate(prev_host, prev_port, reqvec, &callbacks);
  }
  return 0;
}

void print_usage(std::ostream& out)
{
  out << "Usage: spdycat [-Onv] [URI...]" << std::endl;
}

void print_help(std::ostream& out)
{
  print_usage(out);
  out << "\n"
      << "OPTIONS:\n"
      << "    -v, --verbose      Print debug information such as reception/\n"
      << "                       transmission of frames and name/value pairs.\n"
      << "    -n, --null-out     Discard downloaded data.\n"
      << "    -O, --remote-name  Save download data in the current directory.\n"
      << "                       The filename is dereived from URI. If URI\n"
      << "                       ends with '/', 'index.html' is used as a\n"
      << "                       filename. Not implemented yet.\n"
      << std::endl;
}

int main(int argc, char **argv)
{
  while(1) {
    static option long_options[] = {
      {"verbose", no_argument, 0, 'v' },
      {"null-out", no_argument, 0, 'n' },
      {"remote-name", no_argument, 0, 'O' },
      {"help", no_argument, 0, 'h' },
      {0, 0, 0, 0 }
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "Onhv", long_options, &option_index);
    if(c == -1) {
      break;
    }
    switch(c) {
    case 'O':
      config.remote_name = true;
      break;
    case 'h':
      print_help(std::cout);
      exit(EXIT_SUCCESS);
    case 'n':
      config.null_out = true;
      break;
    case 'v':
      config.verbose = true;
      break;
    case '?':
      exit(EXIT_FAILURE);
    default:
      break;
    }
  }
  struct sigaction act;
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, 0);
  SSL_load_error_strings();
  SSL_library_init();
  reset_timer();
  run(argv+optind, argc-optind);
  return 0;
}

} // namespace spdylay

int main(int argc, char **argv)
{
  return spdylay::main(argc, argv);
}