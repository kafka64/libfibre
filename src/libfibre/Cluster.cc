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
#include "libfibre/Cluster.h"

#include <csignal>  // sigaltstack
#include <limits.h> // PTHREAD_STACK_MIN

namespace Context {

static thread_local Fred*          currFred     = nullptr;
static thread_local BaseProcessor* currProc     = nullptr;
static thread_local Cluster*       currCluster  = nullptr;
static thread_local EventScope*    currScope    = nullptr;

#if TESTING_WORKER_POLLER
static thread_local PollerFibre*   workerPoller = nullptr;
#endif

Fred*          CurrFred()       { RASSERT0(currFred);    return  currFred; }
BaseProcessor& CurrProcessor()  { RASSERT0(currProc);    return *currProc; }
Cluster&       CurrCluster()    { RASSERT0(currCluster); return *currCluster; }
EventScope&    CurrEventScope() { RASSERT0(currScope);   return *currScope; }

void setCurrFred(Fred& f, _friend<Fred>) { currFred = &f; }

void install(Fibre* fib, BaseProcessor* bp, Cluster* cl, EventScope* es, _friend<Cluster>) {
  currFred    = fib;
  currProc    = bp;
  currCluster = cl;
  currScope   = es;
#if TESTING_WORKER_POLLER
  workerPoller = new PollerFibre(*es, *bp, bp, "W-Poller   ", false);
  workerPoller->start();
#endif
}

void installFake(EventScope* es, _friend<BaseThreadPoller>) {
  currFred  = (Fred*)0xdeadbeef;
  currScope = es;
}

} // namespace Context

Cluster::Worker::~Worker() {
  if (maintenanceFibre) delete maintenanceFibre;
#ifdef SPLIT_STACK
  if (sigStack) delete [] sigStack;
#endif
}

inline void Cluster::setupWorker(Fibre* fibre, Worker* worker) {
#ifdef SPLIT_STACK
  worker->sigStack = new char[SIGSTKSZ];
  stack_t ss = { .ss_sp = worker->sigStack, .ss_flags = 0, .ss_size = SIGSTKSZ };
  SYSCALL(sigaltstack(&ss, nullptr));
  int off = 0; // do not block signals (blocking signals is slow!)
  __splitstack_block_signals(&off, nullptr);
#endif
  worker->sysThreadId = pthread_self();
  Context::install(fibre, worker, this, &scope, _friend<Cluster>());
  worker->maintenanceFibre = new Fibre(*worker);
  worker->maintenanceFibre->setPriority(Fred::TopPriority);
  worker->maintenanceFibre->run(maintenance, this);
}

void Cluster::initDummy(ptr_t) {}

void Cluster::fibreHelper(Worker* worker) {
  worker->runIdleLoop();
}

void* Cluster::threadHelper(Argpack* args) {
  args->cluster->registerIdleWorker(args->worker, args->initFibre);
  return nullptr;
}

inline void Cluster::registerIdleWorker(Worker* worker, Fibre* initFibre) {
  Fibre* idleFibre = new Fibre(*worker, _friend<Cluster>()); // idle fibre on pthread stack
  setupWorker(idleFibre, worker);
  worker->setIdleLoop(idleFibre);
  Worker::yieldDirect(*initFibre);                           // run init fibre right away
  worker->runIdleLoop();
  idleFibre->endDirect(_friend<Cluster>());
}

void Cluster::preFork(_friend<EventScope>) {
  ScopedLock<WorkerLock> sl(ringLock);
  RASSERT(ringCount == 1, ringCount);
}

void Cluster::postFork1(cptr_t parent, _friend<EventScope>) {
  new (stats) ClusterStats(this, parent);
  for (size_t p = 0; p < iPollCount; p += 1) {
    iPollVec[p].~PollerType();
    new (&iPollVec[p]) PollerType(scope, stagingProc, this, "I-Poller   ");
  }
  for (size_t p = 0; p < oPollCount; p += 1) {
    oPollVec[p].~PollerType();
    new (&oPollVec[p]) PollerType(scope, stagingProc, this, "O-Poller   ");
  }
#if TESTING_WORKER_POLLER
  Context::workerPoller->~PollerFibre();
  new (Context::workerPoller) PollerFibre(Context::CurrEventScope(), Context::CurrProcessor(), this, "W-Poller   ", false);
#endif
}

void Cluster::postFork2(_friend<EventScope>) {
  start();
#if TESTING_WORKER_POLLER
  Context::workerPoller->start();
#endif
}

#if TESTING_WORKER_POLLER
BasePoller& Cluster::getWorkerPoller() {
  return *Context::workerPoller;
}
#endif

Fibre* Cluster::registerWorker(_friend<EventScope>) {
  Worker* worker = new Worker(*this);
  Fibre* mainFibre = new Fibre(*worker, _friend<Cluster>()); // caller continues on pthread stack
  setupWorker(mainFibre, worker);
  Fibre* idleFibre = new Fibre(*worker);                     // idle fibre on new stack
  idleFibre->setup((ptr_t)fibreHelper, worker);              // set up idle fibre for execution
  worker->setIdleLoop(idleFibre);
  return mainFibre;
}

pthread_t Cluster::addWorker(funcvoid1_t initFunc, ptr_t initArg) {
  Worker* worker = new Worker(*this);
  Fibre* initFibre = new Fibre(*worker);
  if (initFunc) {   // run init routine in dedicated fibre, so it can block
    initFibre->setup((ptr_t)initFunc, initArg);
  } else {
    initFibre->setup((ptr_t)initDummy, nullptr);
  }
  Argpack args = { this, worker, initFibre };
  pthread_t tid;
  pthread_attr_t attr;
  SYSCALL(pthread_attr_init(&attr));
  SYSCALL(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));
#if __linux__       // FreeBSD jemalloc segfaults when trying to use minimum stack
  SYSCALL(pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN));
#endif
  SYSCALL(pthread_create(&tid, &attr, (funcptr1_t)threadHelper, &args));
  SYSCALL(pthread_attr_destroy(&attr));
  delete initFibre; // also synchronization that 'args' not needed anymore
  return tid;
}

void Cluster::pause() {
  ringLock.acquire();
  stats->procs.count(ringCount);
  pauseProc = &Context::CurrProcessor();
  for (size_t p = 1; p < ringCount; p += 1) pauseSem.V();
  for (size_t p = 1; p < ringCount; p += 1) confirmSem.P();
}

void Cluster::resume() {
  for (size_t p = 1; p < ringCount; p += 1) sleepSem.V();
  ringLock.release();
}

void Cluster::maintenance(Cluster* cl) {
  for (;;) {
    cl->pauseSem.P();
    cl->stats->sleeps.count();
    cl->confirmSem.V();
    cl->sleepSem.P();
  }
}
