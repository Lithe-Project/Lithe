// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2019 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "TcpStream.h"
#include <System/TcpConnection.h>

namespace System {

TcpStreambuf::TcpStreambuf(TcpConnection& connection) : connection(connection) {
  setg(&readBuf.front(), &readBuf.front(), &readBuf.front());
  setp(reinterpret_cast<char*>(&writeBuf.front()), reinterpret_cast<char *>(&writeBuf.front() + writeBuf.max_size()));
}

TcpStreambuf::~TcpStreambuf() {
  dumpBuffer(true);
}

std::streambuf::int_type TcpStreambuf::overflow(std::streambuf::int_type ch) {
  if (ch == traits_type::eof()) {
    return traits_type::eof();
  }

  if (pptr() == epptr()) {
    if (!dumpBuffer(false)) {
      return traits_type::eof();
    }
  }

  *pptr() = static_cast<char>(ch);
  pbump(1);
  return ch;
}

int TcpStreambuf::sync() {
  return dumpBuffer(true) ? 0 : -1;
}

std::streambuf::int_type TcpStreambuf::underflow() {
  if (gptr() < egptr()) {
    return traits_type::to_int_type(*gptr());
  }

  uint64_t bytesRead;
  try {
    bytesRead = connection.read(reinterpret_cast<uint8_t*>(&readBuf.front()), readBuf.max_size());
  } catch (std::exception&) {
    return traits_type::eof();
  }

  if (bytesRead == 0) {
    return traits_type::eof();
  }

  setg(&readBuf.front(), &readBuf.front(), &readBuf.front() + bytesRead);
  return traits_type::to_int_type(*gptr());
}

bool TcpStreambuf::dumpBuffer(bool finalize) {
  try {
    uint64_t count = pptr() - pbase();
    if(count == 0) {
      return true;
    }

    uint64_t transferred = connection.write(&writeBuf.front(), count);
    if(transferred == count) {
      pbump(-static_cast<int>(count));
    } else {
      if(!finalize) {
        uint64_t front = 0;
        for (uint64_t pos = transferred; pos < count; ++pos, ++front) {
          writeBuf[front] = writeBuf[pos];
        }

        pbump(-static_cast<int>(transferred));
      } else {
        uint64_t offset = transferred;
        while( offset != count) {
          offset += connection.write(&writeBuf.front() + offset, count - offset);
        }

        pbump(-static_cast<int>(count));
      }
    }
  } catch (std::exception&) {
    return false;
  }

  return true;
}

}
