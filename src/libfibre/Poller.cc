/******************************************************************************
    Copyright (C) Martin Karsten 2015-2021

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/
#include "libfibre/Poller.h"
#include "libfibre/EventScope.h"

template<bool Blocking>
inline int BasePoller::doPoll() {
  stats->blocks.count(Blocking);
#if __FreeBSD__
  static const timespec ts = Time::zero();
  int evcnt = kevent(pollFD, nullptr, 0, events, MaxPoll, Blocking ? nullptr : &ts);
#else // __linux__ below
  int evcnt = epoll_wait(pollFD, events, MaxPoll, Blocking ? -1 : 0);
#endif
  if (evcnt < 0) { RASSERT(_SysErrno() == EINTR, _SysErrno()); evcnt = 0; } // gracefully handle EINTR
  DBG::outl(DBG::Level::Polling, "Poller ", FmtHex(this), " got ", evcnt, " events from ", pollFD);
  if (evcnt == 0) stats->empty.count();
  else (Blocking ? stats->eventsB : stats->eventsNB).count(evcnt);
  return evcnt;
}

template<bool Enqueue>
inline Fred* BasePoller::notifyOne(EventType& ev) {
#if __FreeBSD__
  if (ev.filter == EVFILT_READ || ev.filter == EVFILT_TIMER) {
    return eventScope.template unblock<true,Enqueue>(ev.ident, _friend<BasePoller>());
  } else if (ev.filter == EVFILT_WRITE) {
    return eventScope.template unblock<false,Enqueue>(ev.ident, _friend<BasePoller>());
  }
#else // __linux__ below
  if (ev.events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
    return eventScope.unblock<true,Enqueue>(ev.data.fd, _friend<BasePoller>());
  }
  if (ev.events & (EPOLLOUT | EPOLLERR)) {
    return eventScope.unblock<false,Enqueue>(ev.data.fd, _friend<BasePoller>());
  }
#endif
  return nullptr;
}

inline void BasePoller::notifyAll(int evcnt) {
  for (int e = 0; e < evcnt; e += 1) notifyOne(events[e]);
}

#if TESTING_WORKER_POLLER
template<WorkerPoller::PollType PT>
size_t WorkerPoller::internalPoll() {
  int evcnt = (PT == Suspend) ? doPoll<true>() : doPoll<false>();
  notifyAll(evcnt);
  if (PT == Poll) return evcnt;
  if (!eventScope.tryblock(haltFD, _friend<WorkerPoller>())) return 0;
  uint64_t count;
  SYSCALLIO(read(haltFD, (void*)&count, (unsigned)sizeof(count)));
  RASSERT(count == 1, count);
  return 1;
}

size_t WorkerPoller::poll(_friend<Cluster>) {
  return internalPoll<Poll>();
}

size_t WorkerPoller::trySuspend(_friend<Cluster>) {
  return internalPoll<Try>();
}

void WorkerPoller::suspend(_friend<Cluster>) {
  while (internalPoll<Suspend>() == 0);
}
#endif

inline void PollerFibre::pollLoop() {
#if TESTING_POLLER_FIBRE_SPIN
  static const size_t SpinMax = TESTING_POLLER_FIBRE_SPIN;
#else
  static const size_t SpinMax = 1;
#endif
  size_t spin = 1;
  while (!pollTerminate) {
    int evcnt = doPoll<false>();
    if fastpath(evcnt > 0) {
      notifyAll(evcnt);
      Fibre::yieldGlobal();
      spin = 1;
    } else if (spin >= SpinMax) {
      eventScope.blockPollFD(pollFD, _friend<PollerFibre>());
      spin = 1;
    } else {
      Fibre::yieldGlobal();
      spin += 1;
    }
  }
}

void PollerFibre::pollLoopSetup(PollerFibre* This) {
  This->eventScope.registerPollFD(This->pollFD, _friend<PollerFibre>());
  This->pollLoop();
}

PollerFibre::PollerFibre(EventScope& es, BaseProcessor& proc, cptr_t parent, const char* n, _friend<Cluster> fc, bool cluster)
: BasePoller(es, parent, n) {
  pollFibre = new Fibre(proc, fc);
#if TESTING_CLUSTER_POLLER_FLOAT
  if (cluster) pollFibre->setAffinity(Fred::NoAffinity);
#else
  if (cluster) pollFibre->setPriority(Fred::LowPriority);
#endif
}

PollerFibre::~PollerFibre() {
  pollTerminate = true; // set termination flag, then unblock -> terminate
  eventScope.unblockPollFD(pollFD, _friend<PollerFibre>());
  delete pollFibre;
}

void PollerFibre::start() {
  pollFibre->run(pollLoopSetup, this);
}

template<typename T>
inline void BaseThreadPoller::pollLoop(T& This) {
  Context::installFake(&This.eventScope, _friend<BaseThreadPoller>());
  while (!This.pollTerminate) {
    This.prePoll(_friend<BaseThreadPoller>());
    int evcnt = This.template doPoll<true>();
    This.notifyAll(evcnt);
  }
}

void BaseThreadPoller::terminate(_friend<EventScope>) {
  pollTerminate = true;
#if __FreeBSD__
  struct kevent waker;
  EV_SET(&waker, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
  SYSCALL(kevent(pollFD, &waker, 1, nullptr, 0, nullptr));
  EV_SET(&waker, 0, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, 0);
  SYSCALL(kevent(pollFD, &waker, 1, nullptr, 0, nullptr));
  DBG::outl(DBG::Level::Polling, "Poller ", FmtHex(this), " woke ", pollFD);
  SYSCALL(pthread_join(pollThread, nullptr));
#else // __linux__ below
  int waker = SYSCALLIO(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  setupFD(waker, Create, Input, Oneshot);
  uint64_t val = 1;
  val = SYSCALL_EQ(write(waker, &val, sizeof(val)), sizeof(val));
  DBG::outl(DBG::Level::Polling, "Poller ", FmtHex(this), " woke ", pollFD, " via ", waker);
  SYSCALL(pthread_join(pollThread, nullptr));
  SYSCALL(close(waker));
#endif
}

void* PollerThread::pollLoopSetup(void* This) {
  pollLoop(*reinterpret_cast<PollerThread*>(This));
  return nullptr;
}

void* MasterPoller::pollLoopSetup(void* This) {
  pollLoop(*reinterpret_cast<MasterPoller*>(This));
  return nullptr;
}

inline void MasterPoller::prePoll(_friend<BaseThreadPoller>) {
  if (eventScope.tryblock(timerFD, _friend<MasterPoller>())) {
#if __linux__
    uint64_t count; // read timerFD
    if (read(timerFD, (void*)&count, sizeof(count)) != sizeof(count)) return;
#endif
    eventScope.getTimerQueue().checkExpiry();
  }
}
