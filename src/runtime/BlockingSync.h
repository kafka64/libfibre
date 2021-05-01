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
#ifndef _BlockingSync_h_
#define _BlockingSync_h_ 1

#include "runtime/Benaphore.h"
#include "runtime/Debug.h"
#include "runtime/SpinLocks.h"
#include "runtime/Stats.h"
#include "runtime/Fred.h"
#include "runtime-glue/RuntimeContext.h"
#include "runtime-glue/RuntimeLock.h"
#include "runtime-glue/RuntimePreemption.h"
#include "runtime-glue/RuntimeTimer.h"

#include <map>

/****************************** Basics ******************************/

struct Suspender { // funnel suspend calls through this class for access control
  static void prepareRace(Fred& fred) {
    fred.prepareResumeRace(_friend<Suspender>());
  }

  template<bool DisablePreemption=true>
  static ptr_t suspend(Fred& fred) {
    if (DisablePreemption) RuntimeDisablePreemption();
    ptr_t result = fred.suspend(_friend<Suspender>());
    DBG::outl(DBG::Level::Blocking, "Fred ", FmtHex(&fred), " continuing");
    RuntimeEnablePreemption();
    return result;
  }
};

enum SemaphoreResult : size_t { SemaphoreTimeout = 0, SemaphoreSucess = 1, SemaphoreWasOpen = 2 };

/****************************** Timeouts ******************************/

class TimerQueue {
  WorkerLock lock;
  std::multimap<Time,Fred*> queue;
  TimerStats* stats;

public:
  TimerQueue(cptr_t parent = nullptr) { stats = new TimerStats(this, parent); }
  void reinit(cptr_t parent) { new (stats) TimerStats(this, parent); }
  bool empty() const { return queue.empty(); }

  bool checkExpiry(const Time& now, Time& newTime) {
    bool retcode = false;
    int cnt = 0;
    lock.acquire();
    for (auto iter = queue.begin(); iter != queue.end(); ) {
      if (iter->first > now) {
        retcode = true;              // timeouts remaining after this run
        newTime = iter->first - now; // time to next timeout
      break;
      }
      Fred* f = iter->second;
      if (f->raceResume(&queue)) {
        iter = queue.erase(iter);
        f->resume();
      } else {
        iter = std::next(iter);
      }
      cnt += 1;
    }
    lock.release();
    stats->events.count(cnt);
    return retcode;
  }

  // Note that Fred::prepareSuspend must have been called already!
  ptr_t blockTimeout(Fred& cf, const Time& relTimeout, const Time& absTimeout) {
    // set up queue node
    lock.acquire();
    std::multimap<Time,Fred*>::iterator iter = queue.insert( {absTimeout, &cf} );
    if (iter == queue.begin()) Runtime::Timer::newTimeout(relTimeout);
    lock.release();
    // suspend
    ptr_t winner = Suspender::suspend(cf);
    if (winner == &queue) return nullptr; // timer expired
    // clean up
    ScopedLock<WorkerLock> sl(lock);
    queue.erase(iter);
    return winner;                        // timer cancelled
  }
};

static inline bool sleepFred(const Time& timeout, TimerQueue& tq = Runtime::Timer::CurrTimerQueue()) {
  Fred* cf = Context::CurrFred();
  DBG::outl(DBG::Level::Blocking, "Fred ", FmtHex(cf), " sleep ", timeout);
  Suspender::prepareRace(*cf);
  return tq.blockTimeout(*cf, timeout, timeout + Runtime::Timer::now()) == nullptr;
}

/****************************** Common Locked Synchronization ******************************/

class BlockingQueue {
  struct Node : public DoubleLink<Node> { // need separate node for timeout & cancellation
    Fred& fred;
    Node(Fred& cf) : fred(cf) {}
  };
  IntrusiveList<Node> queue;

  ptr_t blockHelper(Fred& cf) {
    return Suspender::suspend(cf);
  }
  ptr_t blockHelper(Fred& cf, const Time& relTimeout, const Time& absTimeout, TimerQueue& tq = Runtime::Timer::CurrTimerQueue()) {
    return tq.blockTimeout(cf, relTimeout, absTimeout);
  }

  template<typename Lock, typename...Args>
  bool blockInternal(Lock& lock, const Args&... args) {
    // set up queue node
    Fred* cf = Context::CurrFred();
    Node node(*cf);
    DBG::outl(DBG::Level::Blocking, "Fred ", FmtHex(cf), " blocking on ", FmtHex(&queue));
    Suspender::prepareRace(*cf);
    queue.push_back(node);
    lock.release();
    // block, potentially with timeout
    ptr_t winner = blockHelper(*cf, args...);
    if (winner == &queue) return true; // blocking completed;
    // clean up
    ScopedLock<Lock> sl(lock);
    queue.remove(node);
    return false;                      // blocking cancelled
  }

  BlockingQueue(const BlockingQueue&) = delete;            // no copy
  BlockingQueue& operator=(const BlockingQueue&) = delete; // no assignment

public:
  BlockingQueue() = default;
  ~BlockingQueue() { RASSERT0(empty()); }
  bool empty() const { return queue.empty(); }

  template<typename Lock>
  bool block(Lock& lock, bool wait = true) {    // Note that caller must hold lock
    if (wait) return blockInternal(lock);
    lock.release();
    return false;
  }

  template<typename Lock>
  bool block(Lock& lock, const Time& timeout) { // Note that caller must hold lock
    Time now = Runtime::Timer::now();
    if (timeout > now) return blockInternal(lock, timeout - now, timeout);
    lock.release();
    return false;
  }

  template<bool Enqueue = true>
  Fred* unblock() {                     // Note that caller must hold lock
    for (Node* node = queue.front(); node != queue.edge(); node = IntrusiveList<Node>::next(*node)) {
      Fred* f = &node->fred;
      if (f->raceResume(&queue)) {
        IntrusiveList<Node>::remove(*node);
        DBG::outl(DBG::Level::Blocking, "Fred ", FmtHex(f), " resume from ", FmtHex(&queue));
        if (Enqueue) f->resume();
        return f;
      }
    }
    return nullptr;
  }
};

template<typename Lock, bool Binary = false, typename BQ = BlockingQueue>
class LockedSemaphore {
  Lock lock;
  volatile ssize_t counter;
  BQ bq;

  void unlock() {}

  template<typename Lock1, typename...Args>
  void unlock(Lock1& l, Args&... args) {
    l.release();
    unlock(args...);
  }

  template<typename... Args>
  SemaphoreResult internalP(const Args&... args) {
    // baton passing: counter unchanged, if blocking fails (timeout)
    if (counter < 1) return bq.block(lock, args...) ? SemaphoreSucess : SemaphoreTimeout;
    counter -= 1;
    lock.release();
    return SemaphoreWasOpen;
  }

public:
  explicit LockedSemaphore(ssize_t c = 0) : counter(c) {}
  ~LockedSemaphore() { cleanup(); }
  void cleanup(ssize_t c = 0) { // baton passing requires serialization at destruction
    ScopedLock<Lock> al(lock);
    RASSERT0(bq.empty());
    counter = c;
  }
  ssize_t getValue() const { return counter; }

  template<typename... Args>
  SemaphoreResult P(const Args&... args) { lock.acquire(); return internalP(args...); }
  SemaphoreResult tryP()                 { lock.acquire(); return internalP(false); }

  template<typename... Args>
  SemaphoreResult unlockP(Args&... args) { lock.acquire(); unlock(args...); return internalP(true); }

  template<bool Enqueue = true, bool TryOnly = false>
  Fred* V() {
    ScopedLock<Lock> al(lock);
    Fred* f = bq.template unblock<Enqueue>();
    if (f) return f;
    if (TryOnly) return nullptr;
    if (Binary) counter = 1;
    else counter += 1;
    return nullptr;
  }

  template<bool Enqueue = true>
  Fred* tryV() { return V<Enqueue,true>(); }

  template<bool Enqueue = true, bool TryOnly = false>
  Fred* release() { return V<Enqueue,TryOnly>(); }
};

template<typename Lock, bool Fifo, typename BQ = BlockingQueue>
class LockedMutex {
  Lock lock;
  Fred* owner;
  BQ bq;

protected:
  template<bool OwnerLock, typename... Args>
  bool internalAcquire(const Args&... args) {
    Fred* cf = Context::CurrFred();
    if (OwnerLock && cf == owner) return true;
    RASSERT(cf != owner, FmtHex(cf), FmtHex(owner));
    for (;;) {
      lock.acquire();
      if (!owner) break;
      if (!bq.block(lock, args...)) return false; // timeout
      if (Fifo) return true; // owner set via baton passing
    }
    owner = cf;
    lock.release();
    return true;
  }

public:
  LockedMutex() : owner(nullptr) {}
  ~LockedMutex() { cleanup(); }
  void cleanup() { // baton passing requires serialization at destruction
    ScopedLock<Lock> al(lock);
    RASSERT(owner == nullptr, FmtHex(owner));
    RASSERT0(bq.empty());
  }

  template<typename... Args>
  bool acquire(const Args&... args) { return internalAcquire<false>(args...); }
  bool tryAcquire() { return acquire(false); }

  void release() {
    ScopedLock<Lock> al(lock);
    RASSERT(owner == Context::CurrFred(), FmtHex(owner));
    if (Fifo) {
      owner = bq.unblock();
    } else {
      owner = nullptr;
      bq.unblock();
    }
  }
};

// condition variable with external lock
template<typename BQ = BlockingQueue>
class Condition {
  BQ bq;

public:
  ~Condition() { cleanup(); }
  void cleanup() { RASSERT0(bq.empty()); }

  template<typename Lock>
  bool wait(Lock& lock) { return bq.block(lock); }

  template<typename Lock>
  bool wait(Lock& lock, const Time& timeout) { return bq.block(lock, timeout); }

  template<bool Broadcast = false>
  void signal() { while (bq.unblock() && Broadcast); }
};

template<typename Lock, typename BQ = BlockingQueue>
class LockedBarrier {
  Lock lock;
  size_t target;
  size_t counter;
  BQ bq;
public:
  explicit LockedBarrier(size_t t = 1) : target(t), counter(0) { RASSERT0(t > 0); }
  ~LockedBarrier() { cleanup(); }
  void cleanup() {
    ScopedLock<Lock> al(lock);
    RASSERT0(bq.empty())
  }

  bool wait() {
    lock.acquire();
    counter += 1;
    if (counter == target) {
      for ( ;counter > 0; counter -= 1) bq.unblock();
      lock.release();
      return true;
    } else {
      bq.block(lock);
      return false;
    }
  }
};

// simple blocking RW lock: release alternates; new readers block when writer waits -> no starvation
template<typename Lock, typename BQ = BlockingQueue>
class LockedRWLock {
  Lock lock;
  ssize_t state;                    // -1 writer, 0 open, >0 readers
  BQ bqR;
  BQ bqW;

  template<typename... Args>
  bool internalAR(const Args&... args) {
    lock.acquire();
    if (state < 0 || !bqW.empty()) {
      if (!bqR.block(lock, args...)) return false;
      lock.acquire();
      bqR.unblock();                // waiting readers can barge after writer
    }
    state += 1;
    lock.release();
    return true;
  }

  template<typename... Args>
  bool internalAW(const Args&... args) {
    lock.acquire();
    if (state != 0) {
      if (!bqW.block(lock, args...)) return false;
      lock.acquire();
    }
    state -= 1;
    lock.release();
    return true;
  }

public:
  LockedRWLock() : state(0) {}
  ~LockedRWLock() { cleanup(); }
  void cleanup() {
    ScopedLock<Lock> al(lock);
    RASSERT(state == 0, state);
    RASSERT0(bqR.empty());
    RASSERT0(bqW.empty());
  }

  template<typename... Args>
  bool acquireRead(const Args&... args) { return internalAR(args...); }
  bool tryAcquireRead() { return acquireRead(false); }

  template<typename... Args>
  bool acquireWrite(const Args&... args) { return internalAW(args...); }
  bool tryAcquireWrite() { return acquireWrite(false); }

  void release() {
    ScopedLock<Lock> al(lock);
    RASSERT0(state != 0);
    if (state > 0) {             // reader leaves; if open -> writer next
      state -= 1;
      if (state > 0) return;
      if (!bqW.unblock()) bqR.unblock();
    } else {                     // writer leaves -> readers next
      RASSERT0(state == -1);
      state += 1;
      if (!bqR.unblock()) bqW.unblock();
    }
  }
};

/****************************** Special Locked Synchronization ******************************/

// synchronization flag with with external lock
template<typename Lock>
class SynchronizedFlag {

public:
  enum State : uintptr_t { Running = 0, Dummy = 1, Posted = 2, Detached = 4 };

protected:
  union {                             // 'waiter' set <-> 'state == Waiting'
    Fred* waiter;
    State state;
  };

public:
  static const State Invalid = Dummy; // dummy bit never set (for ManagedArray)
  explicit SynchronizedFlag(State s = Running) : state(s) {}
  bool posted()   const { return state == Posted; }
  bool detached() const { return state == Detached; }

  bool wait(Lock& lock) {             // returns false, if detached
    if (state == Running) {
      waiter = Context::CurrFred();
      Fred* temp = waiter;            // use a copy, since waiter might be...
      lock.release();                 // ... overwritten after releasing lock
      Suspender::suspend(*temp);
      lock.acquire();                 // reacquire lock to check state
    }
    if (state == Posted) return true;
    if (state == Detached) return false;
    RABORT(FmtHex(state));
  }

  bool post() {                       // returns false, if already detached
    RASSERT0(state != Posted);        // check for spurious posts
    if (state == Detached) return false;
    if (state != Running) waiter->resume();
    state = Posted;
    return true;
  }

  void detach() {                     // returns false, if already posted or detached
    RASSERT0(state != Detached && state != Posted);
    if (state != Running) waiter->resume();
    state = Detached;
  }
};

// synchronization (with result) with external lock
template<typename Runner, typename Result, typename Lock>
class Joinable : public SynchronizedFlag<Lock> {
  using Baseclass = SynchronizedFlag<Lock>;

  union {
    Runner* runner;
    Result  result;
  };

public:
  Joinable(Runner* t) : runner(t) {}

  bool wait(Lock& bl, Result& r) {
    bool retcode = Baseclass::wait(bl);
    r = retcode ? result : 0; // result is available after returning from wait
    return retcode;
  }

  bool post(Result r) {
    bool retcode = Baseclass::post();
    if (retcode) result = r;  // still holding lock while setting result
    return retcode;
  }

  Runner* getRunner() const { return runner; }
};

// synchronization flag with automatic locking
template<typename Lock>
class SyncPoint : public SynchronizedFlag<Lock> {
  using Baseclass = SynchronizedFlag<Lock>;
  typedef typename Baseclass::State State;
  Lock lock;

public:
  SyncPoint(State s = Baseclass::Running) : Baseclass(s) {}
  bool wait()   { ScopedLock<Lock> al(lock); return Baseclass::wait(lock); }
  bool post()   { ScopedLock<Lock> al(lock); return Baseclass::post(); }
  void detach() { ScopedLock<Lock> al(lock); Baseclass::detach(); }
};

/****************************** (Almost-)Lock-Free Synchronization ******************************/

template<typename Lock = DummyLock>
class LimitedSemaphore0 {
  Lock lock;
  FredReadyQueue queue;

public:
  explicit LimitedSemaphore0(ssize_t c = 0) { RASSERT(c == 0, c); }
  ~LimitedSemaphore0() { cleanup(); }
  void cleanup() { RASSERT0(queue.empty()); }

  SemaphoreResult P(bool wait = true) {
    RASSERT0(wait);
    Fred* cf = Context::CurrFred();
    RuntimeDisablePreemption();
    queue.push(*cf);
    Suspender::suspend<false>(*cf);
    return SemaphoreSucess;
  }

  template<bool Enqueue = true, bool DirectSwitch = false>
  Fred* V() {
    Fred* next;
    lock.acquire();
    for (;;) {
      next = queue.pop();
      if (next) break;
      Pause();
    }
    lock.release();
    if (Enqueue) next->resume<DirectSwitch>();
    return next;
  }
};

template<bool DirectSwitch>
class SimpleMutex0 {
  Benaphore<> ben;
  LimitedSemaphore0<> sem;

public:
  SimpleMutex0() : ben(1), sem(0) {}
  bool acquire()    { return ben.P() || sem.P(); }
  bool tryAcquire() { return ben.tryP(); }
  void release()    { if (!ben.V()) sem.V<true,DirectSwitch>(); }
  bool acquire(const Time& timeout) { RABORT("timeout not implementated for SimpleMutex0"); }
};

template<typename Lock = BinaryLock<>>
class FastBarrier {
  size_t target;
  volatile size_t counter;
  FredReadyQueue queue;
  Lock lock;

public:
  explicit FastBarrier(size_t t = 1) : target(t), counter(0) { RASSERT0(t > 0); }
  ~FastBarrier() { cleanup(); }
  void cleanup() { RASSERT0(queue.empty()); }
  bool wait() {
    // There's a race between counter and queue.  A thread can be in a
    // different position in the queue relative to the counter.  The
    // notifier is determined by the counter, so a thread might notify a
    // group of other threads, but itself not continue.  However, one thread
    // of each group must be receive a "special" return code.  With POSIX
    // threads, this is PTHREAD_BARRIER_SERIAL_THREAD - here it's 'true'.
    Fred* cf = Context::CurrFred();
    Suspender::prepareRace(*cf);
    RuntimeDisablePreemption();
    queue.push(*cf);
    bool park = __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED) % target;
    if (!park) {
      __atomic_sub_fetch(&counter, target, __ATOMIC_RELAXED);
      park = true;
      ScopedLock<Lock> sl(lock);
      for (size_t i = 0; i < target; i += 1) {
        Fred* next;
        for (;;) {
          next = queue.pop();
          if (next) break;
          Pause();
        }
        if (next == cf) {
          park = false; // don't suspend self
        } else {
          // if caller ends up suspending, set special return code for last waiter in loop
          if ((i == target - 1) && park) next->raceResume(cf);
          next->resume();
        }
      }
    }
    if (park) return Suspender::suspend<false>(*cf);
    else RuntimeEnablePreemption();
    return cf;
  }
};

/****************************** Compound Types ******************************/

template<typename Semaphore, bool Binary = false>
class FredBenaphore {
  Benaphore<Binary> ben;
  Semaphore sem;

public:
  explicit FredBenaphore(ssize_t c) : ben(c), sem(0) {}
  void cleanup() { sem.cleanup();  }

  SemaphoreResult P()          { return ben.P() ? SemaphoreWasOpen : sem.P(); }
  SemaphoreResult tryP()       { return ben.tryP() ? SemaphoreWasOpen : SemaphoreTimeout; }
  SemaphoreResult P(bool wait) { return wait ? P() : tryP(); }
  SemaphoreResult P(const Time& timeout) { RABORT("timeout not implementated for FredBenaphore"); }

  template<bool Enqueue = true>
  Fred* V() {
    if (ben.V()) return nullptr;
    return sem.template V<Enqueue>();
  }
};

template<typename Semaphore, int SpinStart, int SpinEnd, int SpinCount>
class SpinMutex {
  Fred* volatile owner;
  Semaphore sem;

  template<typename... Args>
  bool tryOnly(const Args&... args) { return false; }

  template<typename... Args>
  bool tryOnly(bool wait) { return !wait; }

  bool tryLock(Fred* cf) {
    Fred* exp = nullptr;
    return __atomic_compare_exchange_n(&owner, &exp, cf, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
  }

protected:
  template<bool OwnerLock, typename... Args>
  bool internalAcquire(const Args&... args) {
    Fred* cf = Context::CurrFred();
    if (OwnerLock && cf == owner) return true;
    RASSERT(cf != owner, FmtHex(cf), FmtHex(owner));
    if (tryOnly(args...)) return tryLock(cf);
    int cnt = 0;
    int spin = SpinStart;
    for (;;) {
      if (tryLock(cf)) return true;
      if (cnt < SpinCount) {
        for (int i = 0; i < spin; i += 1) Pause();
        if (spin < SpinEnd) spin += spin;
        else cnt += 1;
      } else {
        cnt = 0;
        spin = SpinStart;
        if (!sem.P(args...)) return false;
      }
    }
  }

public:
  SpinMutex() : owner(nullptr), sem(1) {}
  ~SpinMutex() { RASSERT(owner == nullptr, FmtHex(owner)); }
  void cleanup() {
    RASSERT(owner == nullptr, FmtHex(owner));
    sem.cleanup();
  }

  template<typename... Args>
  bool acquire(const Args&... args) { return internalAcquire<false>(args...); }
  bool tryAcquire() { return acquire(false); }

  void release() {
    RASSERT(owner == Context::CurrFred(), FmtHex(owner));
    __atomic_store_n(&owner, nullptr, __ATOMIC_RELEASE);
    Fred* next = sem.template V<false>();
    if (next) next->resume();
  }
};

template<typename BaseMutex>
class OwnerMutex : private BaseMutex {
  size_t counter;
  bool recursion;

public:
  OwnerMutex() : counter(0), recursion(false) {}
  void cleanup() { BaseMutex::cleanup(); }
  void enableRecursion() { recursion = true; }

  template<typename... Args>
  size_t acquire(const Args&... args) {
    bool success = recursion
      ? BaseMutex::template internalAcquire<true>(args...)
      : BaseMutex::template internalAcquire<false>(args...);
    if (success) return ++counter; else return 0;
  }
  size_t tryAcquire() { return acquire(false); }

  size_t release() {
    if (--counter > 0) return counter;
    BaseMutex::release();
    return 0;
  }
};

template<typename Lock>
#if TESTING_MUTEX_FIFO
class Mutex : public LockedMutex<Lock, true> {};
#elif TESTING_MUTEX_BARGING
class Mutex : public LockedMutex<Lock, false> {};
#elif TESTING_MUTEX_SPIN
class Mutex : public SpinMutex<LockedSemaphore<Lock, true>, 4, 1024, 16> {};
#else
class Mutex : public SpinMutex<LockedSemaphore<Lock, true>, 0, 0, 0> {};
#endif

#if TESTING_MUTEX_SPIN
typedef SpinMutex<FredBenaphore<LimitedSemaphore0<BinaryLock<>>,true>, 4, 1024, 16> FastMutex;
#else
typedef SpinMutex<FredBenaphore<LimitedSemaphore0<BinaryLock<>>,true>, 0, 0, 0> FastMutex;
#endif

#endif /* _BlockingSync_h_ */
