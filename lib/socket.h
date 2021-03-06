/// \file socket.h
/// A handy Socket wrapper library.
/// Written by Jaron Vietor in 2010 for DDVTech

#pragma once
#include <arpa/inet.h>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef SSL
#include "mbedtls/net.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "mbedtls/error.h"
#endif

// for being friendly with Socket::Connection down below
namespace Buffer{
  class user;
}

/// Holds Socket tools.
namespace Socket{

  void hostBytesToStr(const char *bytes, size_t len, std::string &target);
  bool isBinAddress(const std::string &binAddr, std::string matchTo);
  bool matchIPv6Addr(const std::string &A, const std::string &B, uint8_t prefix);
  std::string getBinForms(std::string addr);

  /// A buffer made out of std::string objects that can be efficiently read from and written to.
  class Buffer{
  private:
    std::deque<std::string> data;

  public:
    std::string splitter;///<String to automatically split on if encountered. \n by default
    Buffer();
    unsigned int size();
    unsigned int bytes(unsigned int max);
    unsigned int bytesToSplit();
    void append(const std::string &newdata);
    void append(const char *newdata, const unsigned int newdatasize);
    void prepend(const std::string &newdata);
    void prepend(const char *newdata, const unsigned int newdatasize);
    std::string &get();
    bool available(unsigned int count);
    std::string remove(unsigned int count);
    std::string copy(unsigned int count);
    void clear();
  };
  // Buffer

  /// This class is for easy communicating through sockets, either TCP or Unix.
  class Connection{
  protected:
    int sock;               ///< Internally saved socket number.
    int pipes[2];           ///< Internally saved file descriptors for pipe socket simulation.
    std::string remotehost; ///< Stores remote host address.
    struct sockaddr_in6 remoteaddr;///< Stores remote host address.
    uint64_t up;
    uint64_t down;
    long long int conntime;
    Buffer downbuffer;                                ///< Stores temporary data coming in.
    virtual int iread(void *buffer, int len, int flags = 0);  ///< Incremental read call.
    virtual unsigned int iwrite(const void *buffer, int len); ///< Incremental write call.
    bool iread(Buffer &buffer, int flags = 0);        ///< Incremental write call that is compatible with Socket::Buffer.
    bool iwrite(std::string &buffer);                 ///< Write call that is compatible with std::string.
  public:
    // friends
    friend class ::Buffer::user;
    // constructors
    Connection();                                              ///< Create a new disconnected base socket.
    Connection(int sockNo);                                    ///< Create a new base socket.
    Connection(std::string hostname, int port, bool nonblock); ///< Create a new TCP socket.
    Connection(std::string adres, bool nonblock = false);      ///< Create a new Unix Socket.
    Connection(int write, int read);                           ///< Simulate a socket using two file descriptors.
    // generic methods
    virtual void close();                    ///< Close connection.
    void drop();                     ///< Close connection without shutdown.
    virtual void setBlocking(bool blocking); ///< Set this socket to be blocking (true) or nonblocking (false).
    bool isBlocking();               ///< Check if this socket is blocking (true) or nonblocking (false).
    std::string getHost() const;     ///< Gets hostname for connection, if available.
    std::string getBinHost();
    void setHost(std::string host); ///< Sets hostname for connection manually.
    int getSocket();                ///< Returns internal socket number.
    int getPureSocket();            ///< Returns non-piped internal socket number.
    std::string getError();         ///< Returns a string describing the last error that occured.
    virtual bool connected() const;         ///< Returns the connected-state for this socket.
    bool isAddress(const std::string &addr);
    bool isLocal(); ///< Returns true if remote address is a local address.
    // buffered i/o methods
    bool spool();                               ///< Updates the downbufferinternal variables.
    bool peek();                                ///< Clears the downbuffer and fills it with peek
    Buffer &Received();                         ///< Returns a reference to the download buffer.
    void SendNow(const std::string &data);      ///< Will not buffer anything but always send right away. Blocks.
    void SendNow(const char *data);             ///< Will not buffer anything but always send right away. Blocks.
    void SendNow(const char *data, size_t len); ///< Will not buffer anything but always send right away. Blocks.
    // stats related methods
    unsigned int connTime();             ///< Returns the time this socket has been connected.
    uint64_t dataUp();                   ///< Returns total amount of bytes sent.
    uint64_t dataDown();                 ///< Returns total amount of bytes received.
    void resetCounter();                 ///< Resets the up/down bytes counter to zero.
    void addUp(const uint32_t i);
    void addDown(const uint32_t i);
    std::string getStats(std::string C); ///< Returns a std::string of stats, ended by a newline.
    friend class Server;
    bool Error;    ///< Set to true if a socket error happened.
    bool Blocking; ///< Set to true if a socket is currently or wants to be blocking.
    // overloaded operators
    bool operator==(const Connection &B) const;
    bool operator!=(const Connection &B) const;
    operator bool() const;
  };

#ifdef SSL
  /// Version of Socket::Connection that uses mbedtls for SSL
  class SSLConnection : public Connection{
    public:
      SSLConnection();
      SSLConnection(std::string hostname, int port, bool nonblock); ///< Create a new TCP socket.
      void close();                    ///< Close connection.
      bool connected() const;         ///< Returns the connected-state for this socket.
      void setBlocking(bool blocking); ///< Set this socket to be blocking (true) or nonblocking (false).
    protected:
      bool isConnected;
      int iread(void *buffer, int len, int flags = 0);  ///< Incremental read call.
      unsigned int iwrite(const void *buffer, int len); ///< Incremental write call.
      mbedtls_net_context * server_fd;
      mbedtls_entropy_context * entropy;
      mbedtls_ctr_drbg_context * ctr_drbg;
      mbedtls_ssl_context * ssl;
      mbedtls_ssl_config * conf;
  };
#endif

  /// This class is for easily setting up listening socket, either TCP or Unix.
  class Server{
  private:
    std::string errors;                                           ///< Stores errors that may have occured.
    int sock;                                                     ///< Internally saved socket number.
    bool IPv6bind(int port, std::string hostname, bool nonblock); ///< Attempt to bind an IPv6 socket
    bool IPv4bind(int port, std::string hostname, bool nonblock); ///< Attempt to bind an IPv4 socket
  public:
    Server();                                                                  ///< Create a new base Server.
    Server(int port, std::string hostname = "0.0.0.0", bool nonblock = false); ///< Create a new TCP Server.
    Server(std::string adres, bool nonblock = false);                          ///< Create a new Unix Server.
    Connection accept(bool nonblock = false);                                  ///< Accept any waiting connections.
    void setBlocking(bool blocking);                                           ///< Set this socket to be blocking (true) or nonblocking (false).
    bool connected() const;                                                    ///< Returns the connected-state for this socket.
    bool isBlocking(); ///< Check if this socket is blocking (true) or nonblocking (false).
    void close();      ///< Close connection.
    void drop();       ///< Close connection without shutdown.
    int getSocket();   ///< Returns internal socket number.
  };

  class UDPConnection{
  private:
    int sock;                   ///< Internally saved socket number.
    std::string remotehost;     ///< Stores remote host address
    void *destAddr;             ///< Destination address pointer.
    unsigned int destAddr_size; ///< Size of the destination address pointer.
    unsigned int up;            ///< Amount of bytes transferred up.
    unsigned int down;          ///< Amount of bytes transferred down.
    unsigned int data_size;     ///< The size in bytes of the allocated space in the data pointer.
    int family;                 ///<Current socket address family
  public:
    char *data;            ///< Holds the last received packet.
    unsigned int data_len; ///< The size in bytes of the last received packet.
    UDPConnection(const UDPConnection &o);
    UDPConnection(bool nonblock = false);
    ~UDPConnection();
    void close();
    int getSock();
    uint16_t bind(int port, std::string iface = "", const std::string &multicastAddress = "");
    void setBlocking(bool blocking);
    void SetDestination(std::string hostname, uint32_t port);
    void GetDestination(std::string &hostname, uint32_t &port);
    uint32_t getDestPort() const;
    bool Receive();
    void SendNow(const std::string &data);
    void SendNow(const char *data);
    void SendNow(const char *data, size_t len);
  };
}

