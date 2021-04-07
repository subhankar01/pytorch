#include <c10d/TCPStore.hpp>

#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#include <fcntl.h>
#include <algorithm>
#include <system_error>

namespace c10d {

namespace {

enum class QueryType : uint8_t {
  SET,
  COMPARE_SET,
  GET,
  ADD,
  CHECK,
  WAIT,
  GETNUMKEYS,
  WATCH_KEY,
  DELETE_KEY
};

enum class CheckResponseType : uint8_t { READY, NOT_READY };

enum class WaitResponseType : uint8_t { STOP_WAITING };

enum class WatchResponseType : uint8_t { KEY_UPDATED };

} // anonymous namespace

// Background thread parent class methods
BackgroundThread::BackgroundThread(int storeListenSocket)
    : storeListenSocket_(storeListenSocket) {
  LOG(ERROR) << "start BackgroundThread()";
  // Signal instance destruction to the daemon thread.
  initStopSignal();
  LOG(ERROR) << "finish BackgroundThread()";
}

BackgroundThread::~BackgroundThread() {
  LOG(ERROR) << "start ~BackgroundThread()";
  // Stop the run
  LOG(ERROR) << "~BackgroundThread.stop()";
  stop();
  // Join the thread
  LOG(ERROR) << "~BackgroundThread.join()";
  join();
  // Close unclosed sockets
  LOG(ERROR) << "~BackgroundThread.closeSocket()";
  for (auto socket : sockets_) {
    if (socket != -1) {
      tcputil::closeSocket(socket);
    }
  }
  // Now close the rest control pipe
  LOG(ERROR) << "~BackgroundThread.closeStopSignal()";
  closeStopSignal();
  LOG(ERROR) << "finish ~BackgroundThread()";
}

void BackgroundThread::join() {
  daemonThread_.join();
}

#ifdef _WIN32
void BackgroundThread::initStopSignal() {
  ghStopEvent_ = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (ghStopEvent_ == NULL) {
    throw std::runtime_error(
        "Failed to create the control pipe to start the "
        "BackgroundThread run");
  }
  LOG(ERROR) << "finish windows initStopSignal()";
}

void BackgroundThread::closeStopSignal() {
  CloseHandle(ghStopEvent_);
}

void BackgroundThread::stop() {
  SetEvent(ghStopEvent_);
}
#else
void BackgroundThread::initStopSignal() {
  if (pipe(controlPipeFd_.data()) == -1) {
    throw std::runtime_error(
        "Failed to create the control pipe to start the "
        "BackgroundThread run");
  }
}

void BackgroundThread::closeStopSignal() {
  for (auto fd : controlPipeFd_) {
    if (fd != -1) {
      ::close(fd);
    }
  }
}

void BackgroundThread::stop() {
  if (controlPipeFd_[1] != -1) {
    // close the write end of the pipe
    ::close(controlPipeFd_[1]);
    controlPipeFd_[1] = -1;
  }
}
#endif

// TCPStoreListener class methods
ListenThread::ListenThread(int listenSocket) : BackgroundThread(listenSocket) {
  daemonThread_ = std::thread(&ListenThread::run, this);
}

void ListenThread::addCallback(
    std::string key,
    std::function<void(std::string, std::string)> cb) {
  keyToCallbacks_[key] = cb;
}

void ListenThread::callbackHandler(int socket) {
  auto watchResponse = tcputil::recvValue<WatchResponseType>(socket);
  std::string key = tcputil::recvString(socket);
  std::vector<uint8_t> currentValueVec = tcputil::recvVector<uint8_t>(socket);
  std::vector<uint8_t> newValueVec = tcputil::recvVector<uint8_t>(socket);
  std::string currentValue =
      std::string(currentValueVec.begin(), currentValueVec.end());
  std::string newValue = std::string(newValueVec.begin(), newValueVec.end());
  if (watchResponse != WatchResponseType::KEY_UPDATED) {
    throw std::runtime_error("KEY_UPDATED response is expected");
  }
  keyToCallbacks_.at(key)(currentValue, newValue);
}

#ifdef _WIN32
void ListenThread::run() {
  std::vector<struct pollfd> fds;
  tcputil::addPollfd(fds, storeListenSocket_, POLLIN);

  bool finished = false;
  while (!finished) {
    // Check control and exit early if triggered
    int res;
    SYSCHECK_ERR_RETURN_NEG1(
        res = WSAPoll(fds.data(), fds.size(), checkTimeout_.count()))
    if (res == 0) {
      auto rv = WaitForSingleObject(ghStopEvent_, 0);
      if (rv != WAIT_TIMEOUT) {
        finished = true;
        break;
      }
      continue;
    }

    // if connection is closed gracefully by master, peeked data will return 0
    char data;
    int ret = recv(fds[1].fd, &data, 1, MSG_PEEK);
    if (ret == 0) {
      continue;
    }

    // valid request, perform callback logic
    callbackHandler(fds[1].fd);
  }
}
#else
void ListenThread::run() {
  std::vector<struct pollfd> fds;
  tcputil::addPollfd(fds, controlPipeFd_[0], POLLHUP);
  tcputil::addPollfd(fds, storeListenSocket_, POLLIN);

  while (1) {
    SYSCHECK_ERR_RETURN_NEG1(::poll(fds.data(), fds.size(), -1));

    // Check control and exit early if triggered
    // The pipe receives an event which tells us to shutdown the listener thread
    if (fds[0].revents != 0) {
      // Will be POLLUP when the pipe is closed
      if (fds[0].revents ^ POLLHUP) {
        throw std::system_error(
            ECONNABORTED,
            std::system_category(),
            "Unexpected poll revent on the control pipe's reading fd: " +
                std::to_string(fds[0].revents));
      }
      break;
    }

    // if connection is closed gracefully by master, peeked data will return 0
    char data;
    int ret = recv(fds[1].fd, &data, 1, MSG_PEEK);
    if (ret == 0) {
      continue;
    }

    // valid request, perform callback logic
    callbackHandler(fds[1].fd);
  }
}
#endif

// TCPStoreDaemon class methods
// Simply start the daemon thread
TCPStoreDaemon::TCPStoreDaemon(int storeListenSocket)
    : BackgroundThread(storeListenSocket) {
  LOG(ERROR) << "start TCPStoreDaemon()";
  daemonThread_ = std::thread(&TCPStoreDaemon::run, this);
  LOG(ERROR) << "finish TCPStoreDaemon()";
}

void TCPStoreDaemon::queryFds(std::vector<struct pollfd>& fds) {
  // Skipping the fds[0] and fds[1],
  // fds[0] is master's listening socket
  // fds[1] is control pipe's reading fd, it is not for Windows platform
  for (size_t fdIdx = CONNECT_SOCKET_OFFSET; fdIdx < fds.size(); ++fdIdx) {
    if (fds[fdIdx].revents == 0) {
      continue;
    }

    // Now query the socket that has the event
    try {
      query(fds[fdIdx].fd);
    } catch (...) {
      // There was an error when processing query. Probably an exception
      // occurred in recv/send what would indicate that socket on the other
      // side has been closed. If the closing was due to normal exit, then
      // the store should continue executing. Otherwise, if it was different
      // exception, other connections will get an exception once they try to
      // use the store. We will go ahead and close this connection whenever
      // we hit an exception here.
      tcputil::closeSocket(fds[fdIdx].fd);

      // Remove all the tracking state of the close FD
      for (auto it = waitingSockets_.begin(); it != waitingSockets_.end();) {
        for (auto vecIt = it->second.begin(); vecIt != it->second.end();) {
          if (*vecIt == fds[fdIdx].fd) {
            vecIt = it->second.erase(vecIt);
          } else {
            ++vecIt;
          }
        }
        if (it->second.size() == 0) {
          it = waitingSockets_.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = watchedSockets_.begin(); it != watchedSockets_.end();) {
        for (auto vecIt = it->second.begin(); vecIt != it->second.end();) {
          if (*vecIt == fds[fdIdx].fd) {
            vecIt = it->second.erase(vecIt);
          } else {
            ++vecIt;
          }
        }
        if (it->second.size() == 0) {
          it = watchedSockets_.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = keysAwaited_.begin(); it != keysAwaited_.end();) {
        if (it->first == fds[fdIdx].fd) {
          it = keysAwaited_.erase(it);
        } else {
          ++it;
        }
      }
      fds.erase(fds.begin() + fdIdx);
      sockets_.erase(sockets_.begin() + fdIdx - CONNECT_SOCKET_OFFSET);
      --fdIdx;
      continue;
    }
  }
}

// query communicates with the worker. The format
// of the query is as follows:
// type of query | size of arg1 | arg1 | size of arg2 | arg2 | ...
// or, in the case of wait
// type of query | number of args | size of arg1 | arg1 | ...
void TCPStoreDaemon::query(int socket) {
  QueryType qt;
  tcputil::recvBytes<QueryType>(socket, &qt, 1);

  if (qt == QueryType::SET) {
    setHandler(socket);

  } else if (qt == QueryType::COMPARE_SET) {
    compareSetHandler(socket);

  } else if (qt == QueryType::ADD) {
    addHandler(socket);

  } else if (qt == QueryType::GET) {
    getHandler(socket);

  } else if (qt == QueryType::CHECK) {
    checkHandler(socket);

  } else if (qt == QueryType::WAIT) {
    waitHandler(socket);

  } else if (qt == QueryType::GETNUMKEYS) {
    getNumKeysHandler(socket);

  } else if (qt == QueryType::DELETE_KEY) {
    deleteHandler(socket);

  } else if (qt == QueryType::WATCH_KEY) {
    watchHandler(socket);

  } else {
    throw std::runtime_error("Unexpected query type");
  }
}

void TCPStoreDaemon::wakeupWaitingClients(const std::string& key) {
  auto socketsToWait = waitingSockets_.find(key);
  if (socketsToWait != waitingSockets_.end()) {
    for (int socket : socketsToWait->second) {
      if (--keysAwaited_[socket] == 0) {
        tcputil::sendValue<WaitResponseType>(
            socket, WaitResponseType::STOP_WAITING);
      }
    }
    waitingSockets_.erase(socketsToWait);
  }
}

void TCPStoreDaemon::sendKeyUpdatesToClients(
    const std::string& key,
    std::vector<uint8_t>& oldData,
    std::vector<uint8_t>& newData) {
  for (int socket : watchedSockets_[key]) {
    tcputil::sendValue<WatchResponseType>(
        socket, WatchResponseType::KEY_UPDATED);
    tcputil::sendString(socket, key, true);
    tcputil::sendVector<uint8_t>(socket, oldData);
    tcputil::sendVector<uint8_t>(socket, newData);
  }
}

void TCPStoreDaemon::setHandler(int socket) {
  std::string key = tcputil::recvString(socket);
  std::vector<uint8_t> newData = tcputil::recvVector<uint8_t>(socket);
  std::vector<uint8_t> oldData;
  if (tcpStore_.find(key) != tcpStore_.end()) {
    oldData = tcpStore_.at(key);
  }
  tcpStore_[key] = newData;
  // On "set", wake up all clients that have been waiting
  wakeupWaitingClients(key);
  // Send key update to all watching clients
  sendKeyUpdatesToClients(key, oldData, newData);
}

void TCPStoreDaemon::compareSetHandler(int socket) {
  std::string key = tcputil::recvString(socket);
  std::vector<uint8_t> currentValue = tcputil::recvVector<uint8_t>(socket);
  std::vector<uint8_t> newValue = tcputil::recvVector<uint8_t>(socket);

  auto pos = tcpStore_.find(key);
  if (pos == tcpStore_.end()) {
    // TODO: This code path is not ideal as we are "lying" to the caller in case
    // the key does not exist. We should come up with a working solution.
    tcputil::sendVector<uint8_t>(socket, currentValue);
  } else {
    if (pos->second == currentValue) {
      pos->second = std::move(newValue);

      // Send key update to all watching clients
      sendKeyUpdatesToClients(key, currentValue, pos->second);
    }
    tcputil::sendVector<uint8_t>(socket, pos->second);
  }
}

void TCPStoreDaemon::addHandler(int socket) {
  std::string key = tcputil::recvString(socket);
  int64_t addVal = tcputil::recvValue<int64_t>(socket);

  std::vector<uint8_t> oldData;
  if (tcpStore_.find(key) != tcpStore_.end()) {
    oldData = tcpStore_[key];
    auto buf = reinterpret_cast<const char*>(tcpStore_[key].data());
    auto len = tcpStore_[key].size();
    addVal += std::stoll(std::string(buf, len));
  }
  auto addValStr = std::to_string(addVal);
  std::vector<uint8_t> newData =
      std::vector<uint8_t>(addValStr.begin(), addValStr.end());
  tcpStore_[key] = newData;
  // Now send the new value
  tcputil::sendValue<int64_t>(socket, addVal);
  // On "add", wake up all clients that have been waiting
  wakeupWaitingClients(key);
  // Send key update to all watching clients
  sendKeyUpdatesToClients(key, oldData, newData);
}

void TCPStoreDaemon::getHandler(int socket) const {
  std::string key = tcputil::recvString(socket);
  auto data = tcpStore_.at(key);
  tcputil::sendVector<uint8_t>(socket, data);
}

void TCPStoreDaemon::getNumKeysHandler(int socket) const {
  tcputil::sendValue<int64_t>(socket, tcpStore_.size());
}

void TCPStoreDaemon::deleteHandler(int socket) {
  std::string key = tcputil::recvString(socket);
  auto numDeleted = tcpStore_.erase(key);
  tcputil::sendValue<int64_t>(socket, numDeleted);
  // Remove all clients watching the key
  watchedSockets_.erase(key);
}

void TCPStoreDaemon::checkHandler(int socket) const {
  SizeType nargs;
  tcputil::recvBytes<SizeType>(socket, &nargs, 1);
  std::vector<std::string> keys(nargs);
  for (size_t i = 0; i < nargs; i++) {
    keys[i] = tcputil::recvString(socket);
  }
  // Now we have received all the keys
  if (checkKeys(keys)) {
    tcputil::sendValue<CheckResponseType>(socket, CheckResponseType::READY);
  } else {
    tcputil::sendValue<CheckResponseType>(socket, CheckResponseType::NOT_READY);
  }
}

void TCPStoreDaemon::waitHandler(int socket) {
  SizeType nargs;
  tcputil::recvBytes<SizeType>(socket, &nargs, 1);
  std::vector<std::string> keys(nargs);
  for (size_t i = 0; i < nargs; i++) {
    keys[i] = tcputil::recvString(socket);
  }
  if (checkKeys(keys)) {
    tcputil::sendValue<WaitResponseType>(
        socket, WaitResponseType::STOP_WAITING);
  } else {
    int numKeysToAwait = 0;
    for (auto& key : keys) {
      // Only count keys that have not already been set
      if (tcpStore_.find(key) == tcpStore_.end()) {
        waitingSockets_[key].push_back(socket);
        numKeysToAwait++;
      }
    }
    keysAwaited_[socket] = numKeysToAwait;
  }
}

void TCPStoreDaemon::watchHandler(int socket) {
  std::string key = tcputil::recvString(socket);

  // record the socket to respond to when the key is updated
  watchedSockets_[key].push_back(socket);
}

bool TCPStoreDaemon::checkKeys(const std::vector<std::string>& keys) const {
  return std::all_of(keys.begin(), keys.end(), [this](const std::string& s) {
    return tcpStore_.count(s) > 0;
  });
}

#ifdef _WIN32
void TCPStoreDaemon::run() {
  std::vector<struct pollfd> fds;
  tcputil::addPollfd(fds, storeListenSocket_, POLLIN);

  // receive the queries
  bool finished = false;
  while (!finished) {
    for (size_t i = 0; i < sockets_.size(); i++) {
      fds[i].revents = 0;
    }

    int res;
    SYSCHECK_ERR_RETURN_NEG1(
        res = WSAPoll(fds.data(), fds.size(), checkTimeout_.count()))
    if (res == 0) {
      auto rv = WaitForSingleObject(ghStopEvent_, 0);
      if (rv != WAIT_TIMEOUT) {
        finished = true;
        break;
      }
      continue;
    }

    // TCPStore's listening socket has an event and it should now be able to
    // accept new connections.
    if (fds[0].revents != 0) {
      if (!(fds[0].revents & POLLIN)) {
        throw std::system_error(
            ECONNABORTED,
            std::system_category(),
            "Unexpected poll revent on the master's listening socket: " +
                std::to_string(fds[0].revents));
      }
      int sockFd = std::get<0>(tcputil::accept(storeListenSocket_));
      sockets_.push_back(sockFd);
      tcputil::addPollfd(fds, sockFd, POLLIN);
    }
    queryFds(fds);
  }
}
#else

void TCPStoreDaemon::run() {
  std::vector<struct pollfd> fds;
  tcputil::addPollfd(fds, storeListenSocket_, POLLIN);
  // Push the read end of the pipe to signal the stopping of the daemon run
  tcputil::addPollfd(fds, controlPipeFd_[0], POLLHUP);

  // receive the queries
  bool finished = false;
  while (!finished) {
    for (size_t i = 0; i < sockets_.size(); i++) {
      fds[i].revents = 0;
    }

    SYSCHECK_ERR_RETURN_NEG1(::poll(fds.data(), fds.size(), -1));

    // TCPStore's listening socket has an event and it should now be able to
    // accept new connections.
    if (fds[0].revents != 0) {
      if (fds[0].revents ^ POLLIN) {
        throw std::system_error(
            ECONNABORTED,
            std::system_category(),
            "Unexpected poll revent on the master's listening socket: " +
                std::to_string(fds[0].revents));
      }
      int sockFd = std::get<0>(tcputil::accept(storeListenSocket_));
      sockets_.push_back(sockFd);
      tcputil::addPollfd(fds, sockFd, POLLIN);
    }

    // The pipe receives an event which tells us to shutdown the daemon
    if (fds[1].revents != 0) {
      // Will be POLLUP when the pipe is closed
      if (fds[1].revents ^ POLLHUP) {
        throw std::system_error(
            ECONNABORTED,
            std::system_category(),
            "Unexpected poll revent on the control pipe's reading fd: " +
                std::to_string(fds[1].revents));
      }
      finished = true;
      break;
    }
    queryFds(fds);
  }
}
#endif

// TCPStore class methods
TCPStore::TCPStore(
    const std::string& masterAddr,
    PortType masterPort,
    c10::optional<int> numWorkers,
    bool isServer,
    const std::chrono::milliseconds& timeout,
    bool waitWorkers)
    : Store(timeout),
      isServer_(isServer),
      tcpStoreAddr_(masterAddr),
      tcpStorePort_(masterPort),
      numWorkers_(numWorkers),
      initKey_("init/"),
      regularPrefix_("/") {
  tcputil::socketInitialize();
  if (isServer_) {
    // Opening up the listening socket
    std::tie(masterListenSocket_, tcpStorePort_) = tcputil::listen(masterPort);
  }
  try {
    if (isServer_) {
      // Now start the daemon
      tcpStoreDaemon_ = std::make_unique<TCPStoreDaemon>(masterListenSocket_);
    }
    // Connect to the daemon
    storeSocket_ = tcputil::connect(
        tcpStoreAddr_, tcpStorePort_, /* wait= */ true, timeout_);
    if (numWorkers.value_or(-1) >= 0 && waitWorkers) {
      waitForWorkers();
    }

// #ifndef _WIN32
    // socket to handle requests from server
    listenSocket_ = tcputil::connect(
        tcpStoreAddr_, tcpStorePort_, /* wait= */ true, timeout_);
    watchListener_ = std::make_unique<ListenThread>(listenSocket_);
// #endif
  } catch (const std::exception&) {
    if (isServer_) {
      tcpStoreDaemon_ = nullptr;
      tcputil::closeSocket(masterListenSocket_);
    }
// #ifndef _WIN32
    watchListener_ = nullptr;
    if (listenSocket_ != -1) {
      tcputil::closeSocket(listenSocket_);
    }
// #endif
    if (storeSocket_ != -1) {
      tcputil::closeSocket(storeSocket_);
    }
    throw;
  }
}

TCPStore::~TCPStore() {
  if (isServer_) {
    // Store daemon should end because of closed connection.
    // daemon destructor should join the thread
    tcpStoreDaemon_ = nullptr;
    tcputil::closeSocket(masterListenSocket_);
  }
  watchListener_ = nullptr;
  tcputil::closeSocket(listenSocket_);
  tcputil::closeSocket(storeSocket_);
}

void TCPStore::waitForWorkers() {
  addHelper_(initKey_, 1);
  // Let server block until all workers have completed, this ensures that
  // the server daemon thread is always running until the very end
  if (isServer_) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
      std::vector<uint8_t> value = getHelper_(initKey_);
      auto buf = reinterpret_cast<const char*>(value.data());
      auto len = value.size();
      int numWorkersCompleted = std::stoi(std::string(buf, len));
      if (numWorkersCompleted >= numWorkers_.value_or(-1)) {
        break;
      }
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start);
      if (timeout_ != kNoTimeout && elapsed > timeout_) {
        break;
      }
      /* sleep override */
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

void TCPStore::set(const std::string& key, const std::vector<uint8_t>& data) {
  std::string regKey = regularPrefix_ + key;
  tcputil::sendValue<QueryType>(storeSocket_, QueryType::SET);
  tcputil::sendString(storeSocket_, regKey, true);
  tcputil::sendVector<uint8_t>(storeSocket_, data);
}

std::vector<uint8_t> TCPStore::compareSet(
    const std::string& key,
    const std::vector<uint8_t>& currentValue,
    const std::vector<uint8_t>& newValue) {
  std::string regKey = regularPrefix_ + key;
  tcputil::sendValue<QueryType>(storeSocket_, QueryType::COMPARE_SET);
  tcputil::sendString(storeSocket_, regKey, true);
  tcputil::sendVector<uint8_t>(storeSocket_, currentValue);
  tcputil::sendVector<uint8_t>(storeSocket_, newValue);
  return tcputil::recvVector<uint8_t>(storeSocket_);
}

std::vector<uint8_t> TCPStore::get(const std::string& key) {
  std::string regKey = regularPrefix_ + key;
  return getHelper_(regKey);
}

std::vector<uint8_t> TCPStore::getHelper_(const std::string& key) {
  waitHelper_({key}, timeout_);
  tcputil::sendValue<QueryType>(storeSocket_, QueryType::GET);
  tcputil::sendString(storeSocket_, key);
  return tcputil::recvVector<uint8_t>(storeSocket_);
}

int64_t TCPStore::add(const std::string& key, int64_t value) {
  std::string regKey = regularPrefix_ + key;
  return addHelper_(regKey, value);
}

bool TCPStore::deleteKey(const std::string& key) {
  std::string regKey = regularPrefix_ + key;
  tcputil::sendValue<QueryType>(storeSocket_, QueryType::DELETE_KEY);
  tcputil::sendString(storeSocket_, regKey);
  auto numDeleted = tcputil::recvValue<int64_t>(storeSocket_);
  return (numDeleted == 1);
}

void TCPStore::watchKey(
    const std::string& key,
    std::function<void(std::string, std::string)> callback) {
// #ifdef _WIN32
//   TORCH_INTERNAL_ASSERT(false, "watchKey not implemented for windows.");
// #else
  std::string regKey = regularPrefix_ + key;

  watchListener_->addCallback(regKey, callback);

  tcputil::sendValue<QueryType>(listenSocket_, QueryType::WATCH_KEY);
  tcputil::sendString(listenSocket_, regKey);
// #endif
}

int64_t TCPStore::addHelper_(const std::string& key, int64_t value) {
  tcputil::sendValue<QueryType>(storeSocket_, QueryType::ADD);
  tcputil::sendString(storeSocket_, key, true);
  tcputil::sendValue<int64_t>(storeSocket_, value);
  return tcputil::recvValue<int64_t>(storeSocket_);
}

int64_t TCPStore::getNumKeys() {
  tcputil::sendValue<QueryType>(storeSocket_, QueryType::GETNUMKEYS);
  return tcputil::recvValue<int64_t>(storeSocket_);
}

bool TCPStore::check(const std::vector<std::string>& keys) {
  tcputil::sendValue<QueryType>(storeSocket_, QueryType::CHECK);
  SizeType nkeys = keys.size();
  tcputil::sendBytes<SizeType>(storeSocket_, &nkeys, 1, (nkeys > 0));
  for (size_t i = 0; i < nkeys; i++) {
    std::string regKey = regularPrefix_ + keys[i];
    tcputil::sendString(storeSocket_, regKey, (i != (nkeys - 1)));
  }
  auto checkResponse = tcputil::recvValue<CheckResponseType>(storeSocket_);
  if (checkResponse == CheckResponseType::READY) {
    return true;
  } else if (checkResponse == CheckResponseType::NOT_READY) {
    return false;
  } else {
    throw std::runtime_error("ready or not_ready response expected");
  }
}

void TCPStore::wait(const std::vector<std::string>& keys) {
  wait(keys, timeout_);
}

void TCPStore::wait(
    const std::vector<std::string>& keys,
    const std::chrono::milliseconds& timeout) {
  std::vector<std::string> regKeys;
  regKeys.resize(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    regKeys[i] = regularPrefix_ + keys[i];
  }
  waitHelper_(regKeys, timeout);
}

void TCPStore::waitHelper_(
    const std::vector<std::string>& keys,
    const std::chrono::milliseconds& timeout) {
  // Set the socket timeout if there is a wait timeout
  if (timeout != kNoTimeout) {
#ifdef _WIN32
    struct timeval timeoutTV = {
        timeout.count() / 1000, (timeout.count() % 1000) * 1000};
#else
    struct timeval timeoutTV = {
        .tv_sec = timeout.count() / 1000,
        .tv_usec = (timeout.count() % 1000) * 1000};
#endif
    SYSCHECK_ERR_RETURN_NEG1(::setsockopt(
        storeSocket_,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<char*>(&timeoutTV),
        sizeof(timeoutTV)));
  }
  tcputil::sendValue<QueryType>(storeSocket_, QueryType::WAIT);
  SizeType nkeys = keys.size();
  tcputil::sendBytes<SizeType>(storeSocket_, &nkeys, 1, (nkeys > 0));
  for (size_t i = 0; i < nkeys; i++) {
    tcputil::sendString(storeSocket_, keys[i], (i != (nkeys - 1)));
  }
  auto waitResponse = tcputil::recvValue<WaitResponseType>(storeSocket_);
  if (waitResponse != WaitResponseType::STOP_WAITING) {
    throw std::runtime_error("Stop_waiting response is expected");
  }
}

const std::string& TCPStore::getHost() const noexcept {
  return tcpStoreAddr_;
}

PortType TCPStore::getPort() const noexcept {
  return tcpStorePort_;
}

} // namespace c10d
