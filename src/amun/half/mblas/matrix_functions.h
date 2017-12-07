#pragma once

#define MAX_THREADS 512
#define MAX_BLOCKS 65535

#include <cmath>
#include <cublas_v2.h>
#include <thrust/execution_policy.h>
#include <thrust/functional.h>
#include <iostream>

#include "thrust_functions.h"
#include "matrix.h"
#include "matrix_wrapper.h"
#include "handles.h"
#include "nth_element_kernels.h"
#include "vector_wrapper.h"

namespace amunmt {
namespace GPUHalf {
namespace mblas {

template <class M>
void Debug(const M& m, size_t pos = 0, size_t l = 8) {
  std::cerr << m.dim(0) << " " << m.dim(1) << std::endl;
  for(size_t i = 0; i < m.dim(0); ++i) {
    std::cerr << i << ": ";
    for(size_t j = pos; j < m.dim(1) && j < pos + l; ++j) {
      std::cerr << m.GetVec()[i * m.dim(1) + j] << " ";
    }
    std::cerr << " ... ";

    for(size_t j = m.dim(1) - l; j < m.dim(1);  ++j) {
      std::cerr << m.GetVec()[i * m.dim(1) + j] << " ";
    }
    std::cerr << std::endl;
    // if(i == 4)
      // break;
  }
}

template<typename T>
std::string Debug(const mblas::Vector<T> &vec, size_t verbosity = 1)
{
  std::stringstream strm;

  strm << "size=" << vec.size();

  if (verbosity) {
    T sum(0);
    for (size_t i = 0; i < vec.size(); ++i) {
      sum += vec[i];
    }
    strm << " sum=" << sum;
  }

  if (verbosity == 2) {
    for (size_t i = 0; i < vec.size(); ++i) {
      strm << " " << vec[i];
    }
  }

  return strm.str();
}

template<typename T>
std::string Debug(const std::vector<T> &vec, size_t verbosity = 1)
{
  std::stringstream strm;

  strm << "size=" << vec.size();

  if (verbosity) {
    T sum = Sum(vec.data(), vec.size());
    strm << " sum=" << sum;
  }

  if (verbosity == 2) {
    for (size_t i = 0; i < vec.size(); ++i) {
      strm << " " << vec[i];
    }
  }

  return strm.str();
}

template<typename T>
void copy(const T *in, size_t count, T *out,  cudaMemcpyKind kind) {
  HANDLE_ERROR( cudaMemcpyAsync(out, in, count * sizeof(T), kind, CudaStreamHandler::GetStream()) );
}

//////////////////////////////////////////////////////////////////////////////////////////////

template<typename T1, typename T2>
__global__ void gCopy(const VectorWrapper<T1> in, VectorWrapper<T2> out)
{
  int id = threadIdx.x + blockIdx.x * blockDim.x;

  if (id < out.size()) {
    T2 val = in[id];
    out[id] = val;
  }
}

template<typename T1, typename T2>
void Copy(const T1 *in, uint size, T2 *out,  cudaMemcpyKind kind)
{
  BEGIN_TIMER("Copy");
  uint threads = std::min((uint)MAX_THREADS, size);
  uint blocks =  (size / threads) + ((size % threads == 0) ?  0 : 1);

  if (kind == cudaMemcpyDeviceToHost) {
    const VectorWrapper<T1> inWrap(in, size);

    Vector<T2> d_out(size);
    VectorWrapper<T2> outWrap(d_out);

    gCopy<<<blocks, threads, 0, CudaStreamHandler::GetStream()>>>(inWrap, outWrap);
    copy(d_out.data(), size, out, cudaMemcpyDeviceToHost);
  }
  else if (kind == cudaMemcpyHostToDevice) {
    Vector<T1> d_in(in, size);
    const VectorWrapper<T1> inWrap(d_in);

    VectorWrapper<T2> outWrap(out, size);

    gCopy<<<blocks, threads ,0, CudaStreamHandler::GetStream()>>>(inWrap, outWrap);
  }
  PAUSE_TIMER("Copy");
}

//////////////////////////////////////////////////////////////////////////////////////////////

void Fill(Matrix& In, float value=0.0f);

Matrix& Swap(Matrix& Out, Matrix& In);

void Mean(Matrix& Out,
          const Matrix& In,
          const mblas::Vector<uint> &sentenceLengths);

void WeightedMean(Matrix& Out,const Matrix& Weights, const Matrix& In, const mblas::Vector<uint>& mapping);

Matrix& Transpose(Matrix& Out, const Matrix& In);

Matrix& Transpose(Matrix& Out);

Matrix& Copy(Matrix& Out, const Matrix& In);

Matrix& PasteRow(Matrix& Out,
                 const Matrix& In,
                 const size_t r = 0,
                 const size_t c = 0);
void PasteRows(Matrix& Out, const Matrix& In, const size_t rowNo, size_t colNo=0);

Matrix& CopyRow(Matrix& Out,
                const Matrix& In,
                const size_t r = 0,
                const size_t c = 0);

Matrix& Concat(Matrix& Out, const Matrix& In);

void MapMatrix(Matrix& state,
              const mblas::Vector<uint> &sentenceLengths,
              size_t i);

Matrix& CopyRows(Matrix& Out,
                 const Matrix& In,
                 const mblas::Vector<uint>& indices);

Matrix& Assemble(Matrix& Out,
                 const Matrix& In,
                 const mblas::Vector<uint>& indices);

Matrix& Slice(Matrix& Out,
              const Matrix& In,
              size_t n, size_t dim);

Matrix& Prod(Matrix& C, const Matrix& A, const Matrix& B,
             bool transB = false);

Matrix& Softmax(Matrix& Out,
                const mblas::Vector<uint>& batchIds,
                const mblas::Vector<uint> &sentenceLengths,
                size_t batchSize);

Matrix& LogSoftmax(Matrix& Out);

template <class Functor>
__global__ void gBroadcast(Functor functor,
                           MatrixWrapper<half> outWrap,
                           const MatrixWrapper<half> in1Wrap,
                           const MatrixWrapper<half> in2Wrap,
                           const VectorWrapper<uint> batchMappingWrap)
{
  int id = threadIdx.x + blockIdx.x * blockDim.x;
  if (id < outWrap.size()) {
    /*
    uint indices[SHAPE_SIZE];
    outWrap.id2Indices(id, indices);

    uint srcId = indices[0];
    uint stateIdx = indices[1];
    uint beamIdx = indices[2];
    //assert(0 == indices[3]);
    */

    uint cols  = in1Wrap.dim(1);
    uint srcSize = outWrap.dim(0);

    uint row = id / cols;
    uint stateIdx = id % cols;
    uint beamIdx = row / srcSize;
    uint srcId = row % srcSize;

    uint batchIdx = batchMappingWrap[ beamIdx ];


    outWrap[id] = functor(in1Wrap[(batchIdx * srcSize + srcId) * cols + stateIdx],
                          in2Wrap[beamIdx * cols + stateIdx]);
    //outWrap[id] = functor(in1Wrap(indices[0], indices[1], 0, batchIdx),
    //                      in2Wrap(indices[2], indices[1], 0, 0));
    //outWrap(srcId, stateIdx, beamIdx, 0) = functor(in1Wrap(srcId, stateIdx, 0, batchIdx),
    //                                              in2Wrap(beamIdx, stateIdx, 0, 0));
    /*
    const half *in1 = &in1Wrap(srcId, stateIdx, 0, batchIdx);
    const half *in2 = &in2Wrap(beamIdx, stateIdx, 0, 0);
    half *out = &outWrap(srcId, stateIdx, beamIdx, 0);
    *out = functor(*in1, *in2);
    */
  }
}

template <class Functor>
Matrix& Broadcast(Functor functor,
                  Matrix& out,
                  const Matrix& in1,
                  const Matrix& in2,
                  const mblas::Vector<uint>& batchMapping,
                  size_t srcSize)
{
  BEGIN_TIMER("Broadcast");
  size_t sumOfBeamSizes = in2.dim(0);

  //size_t rows = srcSize * sumOfBeamSizes;
  size_t cols  = in1.dim(1);

  out.NewSize(srcSize, cols, sumOfBeamSizes);

  MatrixWrapper<half> outWrap(out);
  const MatrixWrapper<half> in1Wrap(in1);
  const MatrixWrapper<half> in2Wrap(in2);
  const VectorWrapper<uint> batchMappingWrap(batchMapping);

  uint size = out.size();
  uint threads = std::min((uint) MAX_THREADS, (uint)size);
  uint blocks  = (size / threads) + ((size % threads == 0) ?  0 : 1);

  gBroadcast<<<blocks, threads, 0, CudaStreamHandler::GetStream()>>>
    (functor, outWrap, in1Wrap, in2Wrap, batchMappingWrap);

  /*
  std::cerr << "nBlocks=" << blocks << std::endl;
  std::cerr << "nThreads=" << threads << std::endl;
  std::cerr << "outWrap=" << outWrap.Debug() << std::endl;
  std::cerr << "in1Wrap=" << in1Wrap.Debug() << std::endl;
  std::cerr << "in2Wrap=" << in2Wrap.Debug() << std::endl;
  std::cerr << "batchMapping=" << batchMapping.Debug() << std::endl;
  std::cerr << "srcSize=" << srcSize << std::endl;
  std::cerr << "sumOfBeamSizes=" << sumOfBeamSizes << std::endl;
  std::cerr << std::endl;
  */
  PAUSE_TIMER("Broadcast");
  return out;
}

template <class Functor>
__global__ void gBroadcastVecColumn(Functor functor,
                                    MatrixWrapper<half> outWrap,
                                    const VectorWrapper<half> inWrap) {
  extern __shared__ half sdataOrig[];

  size_t rows  = outWrap.dim(0);
  size_t cols = outWrap.dim(1);

  VectorWrapper<half> sdata(sdataOrig, rows);

  if (threadIdx.x == 0) {
    for (int i = 0; i < rows; ++i)
      sdata[i] = inWrap[i];
  }
  __syncthreads();

  int noColumn = threadIdx.x + blockDim.x * blockIdx.x;
  if (noColumn < cols) {
    for (int noRow = 0; noRow < rows; ++noRow) {
      half &val = outWrap(noRow, noColumn, 0, 0);
      val = functor(val, sdata[noRow]);
    }
  }
}

template <class Functor>
Matrix& BroadcastVecColumn(Functor functor, Matrix& Out, const mblas::Vector<half>& In)
{
  BEGIN_TIMER("BroadcastVecColumn");
  size_t rows  = Out.dim(0);
  size_t cols = Out.dim(1);

  MatrixWrapper<half> outWrap(Out);
  const VectorWrapper<half> inWrap(In);

  int threads = std::min(MAX_THREADS, (int)cols);
  int blocks  = cols / threads  + ((cols % threads == 0) ?  0 : 1);

  gBroadcastVecColumn<<<blocks, threads, rows * sizeof(half), CudaStreamHandler::GetStream()>>>
    (functor, outWrap, inWrap);

  PAUSE_TIMER("BroadcastVecColumn");
  return Out;
}

template <class Functor>
__global__ void gBroadcastVec(Functor functor,
                              MatrixWrapper<half> outWrap,
                              const MatrixWrapper<half> inWrap)
{
  size_t cols = outWrap.dim(1);

  int noColumn = threadIdx.x + blockDim.x * blockIdx.x;
  if (noColumn < cols) {
    half vecValue = inWrap(0, noColumn, 0, 0);

    for (int dim0 = 0; dim0 < outWrap.dim(0); ++dim0) {
      for (int dim2 = 0; dim2 < outWrap.dim(2); ++dim2) {
        for (int dim3 = 0; dim3 < outWrap.dim(3); ++dim3) {
          half &val = outWrap(dim0, noColumn, dim2, dim3);
          val = functor(val, vecValue);
        }
      }
    }

  }
}

template <class Functor>
Matrix& BroadcastVec(Functor functor, Matrix& Out, const Matrix& In)
{
  BEGIN_TIMER("BroadcastVec");
  //std::cerr << "Out=" << Out.Debug() << std::endl;
  //std::cerr << "In=" << In.Debug() << std::endl;

  size_t cols = Out.dim(1);

  MatrixWrapper<half> outWrap(Out);
  const MatrixWrapper<half> inWrap(In);

  int threads = std::min(MAX_THREADS, (int)cols);
  int blocks  = cols / threads  + ((cols % threads == 0) ?  0 : 1);
  const cudaStream_t& stream = CudaStreamHandler::GetStream();

  gBroadcastVec<<<blocks, threads, 0, stream>>>
    (functor, outWrap, inWrap);

  PAUSE_TIMER("BroadcastVec");
  return Out;
}

template <class Functor>
__global__ void gElement(Functor functor,
                         MatrixWrapper<half> outWrap)
{
  size_t ind = blockIdx.x * blockDim.x + threadIdx.x;
  if (ind < outWrap.size()) {
    outWrap[ind] = functor(outWrap[ind]);
  }
}

template <class Functor>
Matrix& Element(Functor functor,
                Matrix& Out)
{
  BEGIN_TIMER("Element1");
  uint size = Out.size();
  uint threads = std::min((uint) MAX_THREADS, (uint)size);
  uint blocks  = size / threads + ((size % threads == 0) ?  0 : 1);
  const cudaStream_t& stream = CudaStreamHandler::GetStream();

  MatrixWrapper<half> outWrap(Out);

  gElement<<<blocks, threads, 0, stream>>>
    (functor, outWrap);

  PAUSE_TIMER("Element1");
  return Out;
}

template <class Functor>
__global__ void gElement(Functor functor,
                         MatrixWrapper<half> outWrap,
                         const MatrixWrapper<half> inWrap)
{
  size_t ind = blockIdx.x * blockDim.x + threadIdx.x;
  if (ind < outWrap.size()) {
    outWrap[ind] = functor(outWrap[ind], inWrap[ind]);
  }
}

template <class Functor>
Matrix& Element(Functor functor,
                Matrix& Out, const Matrix& In)
{
  BEGIN_TIMER("Element2");
  assert(Out.size() == In.size());

  uint size = Out.size();
  uint threads = std::min((uint) MAX_THREADS, (uint)size);
  uint blocks  = size / threads + ((size % threads == 0) ?  0 : 1);
  const cudaStream_t& stream = CudaStreamHandler::GetStream();

  MatrixWrapper<half> outWrap(Out);
  const MatrixWrapper<half> inWrap(In);

  gElement<<<blocks, threads, 0, stream>>>
    (functor, outWrap, inWrap);

  PAUSE_TIMER("Element2");
  return Out;
}

template <class Functor>
__global__ void gElement(Functor functor,
                         MatrixWrapper<half> outWrap,
                         const MatrixWrapper<half> in1Wrap,
                         const MatrixWrapper<half> in2Wrap)
{
  size_t ind = blockIdx.x * blockDim.x + threadIdx.x;
  if (ind < outWrap.size()) {
    outWrap[ind] = functor(outWrap[ind], in1Wrap[ind], in2Wrap[ind]);
  }
}

template <class Functor>
Matrix& Element(Functor functor,
                Matrix& Out, const Matrix& In1, const Matrix& In2)
{
  BEGIN_TIMER("Element3");
  //std::cerr << "Out=" << Out.Debug() << std::endl;
  //std::cerr << "In1=" << In1.Debug() << std::endl;
  //std::cerr << "In2=" << In2.Debug() << std::endl;

  assert(Out.size() == In1.size());
  assert(Out.size() == In2.size());

  uint size = Out.size();
  uint threads = std::min((uint) MAX_THREADS, (uint)size);
  uint blocks  = size / threads + ((size % threads == 0) ?  0 : 1);
  const cudaStream_t& stream = CudaStreamHandler::GetStream();

  //std::cerr << "Element3=" << Out.Debug(0) << std::endl;
  //std::cerr << "Element3=" << In1.Debug(0) << std::endl;
  //std::cerr << "Element3=" << In2.Debug(0) << std::endl;
  //std::cerr << std::endl;
  MatrixWrapper<half> outWrap(Out);
  const MatrixWrapper<half> in1Wrap(In1);
  const MatrixWrapper<half> in2Wrap(In2);
  //std::cerr << "outWrap=" << outWrap.Debug() << std::endl;

  gElement<<<blocks, threads, 0, stream>>>
    (functor, outWrap, in1Wrap, in2Wrap);

  //HANDLE_ERROR( cudaPeekAtLastError() );
  //HANDLE_ERROR( cudaDeviceSynchronize() );
  //HANDLE_ERROR( cudaPeekAtLastError() );

  PAUSE_TIMER("Element3");
  return Out;
}

void SetColumn(Matrix& In, int noColumn, float value);

void Normalization(Matrix& out, const Matrix& in, const Matrix& alpha, const Matrix& beta,
                   float eps);

void Normalization(Matrix& out, const Matrix& in, const Matrix& alpha, float eps);

void LogSoftmaxAndNBest(mblas::Vector<NthOutBatch> &nBest,
                const Matrix& in,
                const Matrix& b4,
                const mblas::Vector<half> &costs,
                bool forbidUNK,
                uint maxBeamSize,
                const std::vector<uint>& beamSizes,
                uint beamSizeSum,
                bool isFirst);

template<typename T>
void TestMemCpy(size_t size, const T *data1)
{
  using namespace std;

  vector<T> h_vec2(size);

  T *d_vec;
  cudaMalloc(&d_vec, size * sizeof(T));

  // copy
  //cudaMemcpy(d_vec, h_vec1.data(), NUM * sizeof(float), cudaMemcpyHostToDevice);
  //cudaMemcpy(h_vec2.data(), d_vec, NUM * sizeof(float), cudaMemcpyDeviceToHost);

  cudaStream_t stream = mblas::CudaStreamHandler::GetStream();

  //cudaMemcpyAsync(d_vec, data1, size * sizeof(T), cudaMemcpyHostToDevice, stream);
  //cudaMemcpyAsync(h_vec2.data(), d_vec, size * sizeof(T), cudaMemcpyDeviceToHost, stream);

  mblas::copy(data1, size, d_vec, cudaMemcpyHostToDevice);
  mblas::copy(d_vec, size, h_vec2.data(), cudaMemcpyDeviceToHost);

  cerr << "h_vec2=";
  T sum = 0;
  for (size_t i = 0; i < size; ++i) {
    //cerr << h_vec2[i] << " ";
    sum += h_vec2[i];
  }
  cerr << sum;
  cerr << endl;
  //cudaStreamDestroy(stream);
  cudaFree(d_vec);

}

void TestMemCpy();

void CopyNthOutBatch(const mblas::Vector<NthOutBatch> &nBest,
              std::vector<uint>& outKeys,
              std::vector<float>& outValues);

} // namespace mblas
} // namespace GPU
}