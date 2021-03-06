// @TODO: rename to communicator_nccl.h
// Note: This must only be included if defined(CUDA_FOUND) && defined(USE_NCCL)
#include "training/communicator.h"
#include "3rd_party/threadpool.h"
#include "tensors/gpu/cuda_helpers.h"

#include "cuda_runtime.h"
#include "nccl.h"
#if (NCCL_MAJOR<3 || NCCL_MINOR<2)
#define ncclGetVersion(pv) (*(pv) = (NCCL_MAJOR * 1000 + NCCL_MINOR * 100 + NCCL_PATCH))
#endif

#include <signal.h> // HACK
#include <sys/types.h>
#include <sys/syscall.h>
pid_t gettid(void){ return syscall(SYS_gettid); }

namespace marian {

class NCCLCommunicator : public ICommunicator {
private:
  std::vector<ncclComm_t> comms_;     // [device index]
  std::vector<cudaStream_t> streams_; // [device index]
  std::vector<int> devices_;          // [device index]
  Ptr<IMPIWrapper> mpi_; // (may be null)
  mutable ThreadPool threadPool_;
  mutable std::vector<std::future<void>> threadResults_; // [device index]

  void groupStart() const { NCCL_CHECK(ncclGroupStart()); } // helpers to make sure we check the error
  void groupEnd()   const { NCCL_CHECK(ncclGroupEnd());   }

  void synchronizeAll() const {
    for(int i = 0; i < graphs_.size(); ++i) {
      CUDA_CHECK(cudaSetDevice(devices_[i]));
      CUDA_CHECK(cudaStreamSynchronize(streams_[i]));
      // @TODO: why do we sync the CPU, and not the GPU?
      //  - cudaEventRecord() an event on the nccl stream
      //  - submit a cudaStreamWaitEvent() into our compute stream (=NULL stream)
      // cf. https://github.com/pytorch/pytorch/blob/master/torch/lib/c10d/ProcessGroupNCCL.cpp
    }
  }

  std::string mpiIdStr() const { // (for logging)
    return mpi_ ? mpi_->idStr() : "";
  }

  size_t myNcclRank(size_t localDeviceIndex) const { // map local device index to a global rank
    if (mpi_)
      return mpi_->myMPIRank() * devices_.size() + localDeviceIndex;
    else
      return localDeviceIndex;
  }

  size_t numNcclRanks() const { // total number of devices across all MPI processes
    if (mpi_)
      return mpi_->numMPIProcesses() * devices_.size();
    else
      return devices_.size();
  }

  size_t dataSize() const { // total number of floats that comprise the concatenated parameter and gradient vector
    return graphs_[0]->params()->vals()->size();
  }

  // determine the (max) shard size
  // All shards except the last one have this size.
  // Presently, even all shards must have identical size, due to a limitation in NCCL we have not yet worked around.
  size_t shardSize() const {
    size_t numShards = numNcclRanks();
    size_t size = (dataSize() + numShards - 1) / numShards;
#if 1 // for now, all shards must have the same size, since NCCL does not allow a sub-slice for the last shard
    ABORT_IF(size * numShards != dataSize(), "presently, all shards must have the same size");
#endif
    return size;
  }

  // determine the index range (begin, end) of a shard
  std::pair<size_t, size_t> ncclRankShardRange(size_t rank) const {
    size_t size = shardSize();
    size_t begin = rank * size;
    size_t end = begin + size;
    end = std::min(end, dataSize()); // clip last shard. Note: Presently this never happens, since shardSize() enforces uniform shard size.
    return{ begin, end };
  }

  // determine the index range (begin, end) of a shard
  std::pair<size_t, size_t> localShardRange(size_t localDeviceIndex) const {
    return ncclRankShardRange(myNcclRank(localDeviceIndex));
  }

  static std::string ncclVersionString() {
    int ncclVersion = 0;
    ncclGetVersion(&ncclVersion);
    return std::to_string(ncclVersion/1000) + "." + std::to_string((ncclVersion/100)%10) + "." + std::to_string(ncclVersion%100);
  }

  void mpiBarrier() const {
    if (mpi_)
      mpi_->barrier();
  }

  // helper class to temporarily block a UNIX signal
  class BlockSignal {
    typedef std::function<void(int, const sigset_t*, sigset_t*)> SigMaskFn;
    SigMaskFn sigMaskFn_; // function to set the mask, thread or proc
    sigset_t oldSigSet_;  // old set to restore the signal
  public:
    BlockSignal(int signal, const SigMaskFn& sigMaskFn) : sigMaskFn_(sigMaskFn) {
      sigset_t newSigSet;
      sigemptyset(&newSigSet);
      sigaddset(&newSigSet, signal); // block signal by setting it in the blocked-signal mask
      sigMaskFn_(SIG_BLOCK, &newSigSet, &oldSigSet_);
    }
    ~BlockSignal() {
      sigMaskFn_(SIG_BLOCK, &oldSigSet_, nullptr); // restore old signal mask
    }
  };

public:
  // a NCCLCommunicator is bound to a set of graphs, one per GPU device
  // If MPI is used, then each MPI process has an instance of this class for its specific
  // set of GPU devices, which are communicating with each other. The total number of GPUs
  // involved in the NCCL communication setup is (#MPI processes) x (#GPUs per process).
  NCCLCommunicator(const std::vector<Ptr<ExpressionGraph>>& graphs, Ptr<IMPIWrapper> mpi)
      : ICommunicator(graphs),
        comms_(graphs.size()),
        streams_(graphs.size()),
        devices_(graphs.size()),
        mpi_(mpi),
        threadPool_(graphs.size(), graphs.size()), threadResults_(graphs.size()) {
    mpiBarrier(); // barrier to group the multiple log messages from MPI processes
    LOG(info, "[comm] Using NCCL {} {}for GPU communication",
        ncclVersionString(),
        (mpi_ && mpi_->numMPIProcesses() > 1) ? "and MPI " : "");
    mpiBarrier(); // (synchronize the log messages)

    // set up our local devices
    for(int i = 0; i < graphs_.size(); ++i) {
      auto device = graphs_[i]->getBackend()->getDeviceId();

      ABORT_IF(device.type != DeviceType::gpu,
               "NCCL communicator can only be used with GPUs");

      devices_[i] = device.no;
      CUDA_CHECK(cudaSetDevice(devices_[i]));
      CUDA_CHECK(cudaStreamCreate(&streams_[i]));
    }

    // set up NCCL
    // Since we want to use MPI, we cannot use NCCL's handy convenience function. Instead, we must go the laborious route.
    // cf. https://docs.nvidia.com/deeplearning/sdk/nccl-developer-guide/index.html#multidevprothrd

    // generate NCCL unique ID at one process and broadcast to all
    ncclUniqueId uniqueId = { 0 };
    if (!mpi_ || mpi->myMPIRank() == 0)
      NCCL_CHECK(ncclGetUniqueId(&uniqueId));

    if (mpi_) {
      //LOG(info, "[{}] before bcast", mpiIdStr());
      static_assert(sizeof(uniqueId) == NCCL_UNIQUE_ID_BYTES, "wrong NCCL_UNIQUE_ID_BYTES??"); // (this value is used in NVidia examples)
      mpi_->bCast(&uniqueId, sizeof(uniqueId), MPI_BYTE, 0);
      //LOG(info, "[{}] after bcast", mpiIdStr());
    }

    //mpiBarrier(); // should not be needed since bCast is a barrier

    // Note: due to a bug in NCCL 2.3.5, NCCL's allocation of shared memory intermittently fails with
    //          Failed, NCCL error 2 'unhandled system error' - ncclGroupEnd()
    //          include/shm.h:26 NCCL WARN Unable to allocate shared memory (4263936 bytes) : Interrupted system call
    // This is caused by SIGPROF signals being raised, causing EINTR, which NCCL does not handle.
    // Reported as Issue #137 on the NCCL Github.
    // To work around, we disable the SIGPROF signal during NCCL initialization.
#define SIG_BAD 27 // SIGPROF
    BlockSignal blockThread(SIG_BAD, pthread_sigmask); // Note: I don't know yet which of these two makes the difference.
    BlockSignal blockProc(SIG_BAD, sigprocmask);       // So for now just block both.

    groupStart();
    for (int localDeviceIndex = 0; localDeviceIndex < devices_.size(); localDeviceIndex++) {
      CUDA_CHECK(cudaSetDevice(devices_[localDeviceIndex]));
      //LOG(info, "[{}] ncclCommInitRank {} out of {}: GPU[{}]", mpiIdStr(), myNcclRank(localDeviceIndex), numNcclRanks(), localDeviceIndex);
      NCCL_CHECK(ncclCommInitRank(&comms_[localDeviceIndex], numNcclRanks(), uniqueId, myNcclRank(localDeviceIndex)));
      //LOG(info, "[{}] done ncclCommInitRank {} out of {}, GPU[{}]", mpiIdStr(), myNcclRank(localDeviceIndex), numNcclRanks(), localDeviceIndex);
    }
    groupEnd();

    mpiBarrier(); // (synchronize the log messages)
    LOG(debug, "NCCLCommunicator constructed successfully for {}", mpiIdStr());
    mpiBarrier(); // (synchronize the log messages)
  }

  ~NCCLCommunicator() override {
    for(int i = 0; i < devices_.size(); ++i) {
      cudaSetDevice(devices_[i]);
      cudaStreamDestroy(streams_[i]);
      ncclCommDestroy(comms_[i]);
    }
  }

  void foreach(const ForeachFunc& func, bool parallel = true) const override {
    parallel &= graphs_.size() > 1;

    for(size_t i = 0; i < graphs_.size(); ++i) {
      size_t begin, end; std::tie
      (begin, end) = localShardRange(i);
      //std::cerr << "[" << mpiIdStr() << "] foreach " << begin << " " << end << std::endl;
try{
      if (parallel)
        threadResults_[i] = threadPool_.enqueue(func, i, begin, end);
        //group.emplace_back(func, i, begin, end);
        //threadPool_.enqueue([&](size_t i){
        //  func(i, begin, end);
        //}, i);
      else
        func(i, begin, end);
}
catch (const std::exception& e) // something leaks thread handles
{
  // keeping this around, in case the error still happens  --@TODO: remove once this has not been observed anymore
  LOG(info, "caught exception in foreach {}", i);
  system("ps -T -A");
  throw;
}
    }
    if (parallel)
      for(size_t i = 0; i < graphs_.size(); ++i)
        threadResults_[i].wait();
    //for(auto& t : group) // (note: group is empty is not parallel)
    //  t.join();
  }

  void scatterReduce() const override {
    groupStart();
    for(int i = 0; i < graphs_.size(); ++i) {
      size_t begin, end; std::tie
      (begin, end) = localShardRange(i);
      //std::cerr << "[" << mpiIdStr() << "] scatterReduce " << begin << " " << end << std::endl;

      auto grads = graphs_[i]->params()->grads();
      const auto* sendbuf = grads->data();
      auto*       recvbuf = grads->subtensor(begin, end-begin)->data();
      size_t      bufsize = shardSize();

      NCCL_CHECK(ncclReduceScatter(sendbuf, recvbuf, bufsize, ncclFloat, ncclSum, comms_[i], streams_[i]));
    }
    groupEnd();
    //std::cerr << "scatterReduce submitted" << std::endl;
    synchronizeAll();
    //std::cerr << "scatterReduce completed" << std::endl;
  }

  // This distributes all 64 model shards to all 64 GPUs.
  // @TODO: For unknown reasons, this takes longer than any other operation incl. scatterReduce().
  //        But both should have the same number of data transfers of the same size.
  void allGather() const override {
    groupStart();
    for(int i = 0; i < graphs_.size(); ++i) {
      size_t begin, end; std::tie
      (begin, end) = localShardRange(i);
      //std::cerr << "[" << mpiIdStr() << "] allGather " << begin << " " << end << std::endl;

      auto vals = graphs_[i]->params()->vals();
      const auto* sendbuf = vals->subtensor(begin, end-begin)->data();
      void*       recvbuf = vals->data();
      size_t      bufsize = shardSize();

      NCCL_CHECK(ncclAllGather(sendbuf, recvbuf, bufsize, ncclFloat, comms_[i], streams_[i]));
    }
    groupEnd();
    synchronizeAll();
  }

  // swap distributed paramShards with model params()
  // It is assumed that all model params() on all devices and MPI processes are identical.
  // This is used for the smoothed parameters.
  void swapParams(const std::vector<Tensor>& distributedParamShards) const override {
    // get everything onto the CPU
    auto distributedParams = gatherState([&](size_t localDeviceIndex) {
      std::vector<float> tmp;
      distributedParamShards[localDeviceIndex]->get(tmp);
      //LOG(info, "[{}] swapParams.getFn({}) -> size {}, ({}, {}, {}, ...)", mpiIdStr(), localDeviceIndex, tmp.size(), tmp[0], tmp[1], tmp[2]);
      return tmp;
    });
    // Now all MPI processes hold an identical copy of a concatenation of all distributedParamShards[] across local and remote devices.
    std::vector<float> localParams;
    graphs_[0]->params()->vals()->get(localParams);
    // Now all MPI processes hold an identical copy of params() (remember, we assumed all devices hold the same params()).
    //LOG(info, "[{}] swapParams: distributedParams.size = {}, localParams.size = {}", mpiIdStr(), distributedParams.size(), localParams.size());
    ABORT_IF(distributedParams.size() != localParams.size(), "distributed sharded and local params have different size??");

    // swap
    std::swap(distributedParams, localParams);

    // distribute it back
    scatterState(distributedParams, [&](size_t localDeviceIndex,
                                        std::vector<float>::const_iterator begin,
                                        std::vector<float>::const_iterator end){
      ABORT_IF(distributedParamShards[localDeviceIndex]->size() != end-begin, "swapParams size mismatch??"); // @TODO: move check to set()
      distributedParamShards[localDeviceIndex]->set(std::vector<float>(begin, end)); // @TODO: directly pass iterators to set()
    });
    for (auto& graph : graphs_) // broadcast to every local graph
      graph->params()->vals()->set(localParams);
  }

  // Distribute a single CPU-side vector to shards across multiple devices and MPI processes.
  // This is used when restoring optimizer state, which is sharded, and as part of swapParams().
  // It is assumed that all MPI processes get the same data() passed. Hence, no MPI transfers are needed here.
  void scatterState(const std::vector<float>& data, const OptimizerBase::ScatterStateSetFunc& setFn) const override {
    size_t dataSize = data.size();
    size_t numShards = numNcclRanks();
    size_t shardSize = (dataSize + numShards - 1) / numShards;
    for(size_t localDeviceIndex = 0; localDeviceIndex < graphs_.size(); localDeviceIndex++) {
      // We only slice out data that is kept in our MPI process. Remember that all MPI processes receive the same, complete data.
      auto ncclRank = myNcclRank(localDeviceIndex);
      size_t begin = ncclRank * shardSize;
      size_t end   = std::min(begin + shardSize, dataSize);
      setFn(localDeviceIndex, data.begin() + begin, data.begin() + end);
    }
  }

  // Collect shards across multiple devices and MPI processes in the NCCL configuration into a single CPU-side vector.
  // This is used when persisting optimizer state, which is sharded, and as part of swapParams().
  std::vector<float> gatherState(const OptimizerBase::GatherStateGetFunc& getFn) const override {
    std::vector<float> tmp; // (temp buffer used multiple times)
    // first, concatenate over all local devices
    std::vector<float> localData;
    for(size_t localDeviceIndex = 0; localDeviceIndex < graphs_.size(); localDeviceIndex++) {
      tmp = getFn(localDeviceIndex);
      localData.insert(localData.end(), tmp.begin(), tmp.end());
    }
    //LOG(info, "[{}] gatherState: localData.size = {}", mpiIdStr(), localData.size());
    // second, concatenate across MPI processes
    // Note that all local devices occupy consecutive ncclRanks in order.
    std::vector<float> data;
    if (mpi_) {
      // push one rank's data at a time using broadcast
      for(size_t mpiRank = 0; mpiRank < mpi_->numMPIProcesses(); mpiRank++) {
        // broadcast mpiRank's localData to all
        if(mpiRank == mpi_->myMPIRank())
          tmp = localData;
        mpi_->bCast(tmp, /*rootRank=*/mpiRank);
        // now all ranks have the same slice: concatenate (we will end up with the same on all MPI processes)
        data.insert(data.end(), tmp.begin(), tmp.end());
      }
    }
    else { // no MPI: localData is the complete result already
      data = std::move(localData);
    }
    return data;
  }
};

}  // namespace marian
