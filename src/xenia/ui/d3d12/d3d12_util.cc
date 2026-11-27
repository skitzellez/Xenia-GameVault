/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/d3d12_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"

namespace xe {
namespace ui {
namespace d3d12 {
namespace util {

constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesDefault = {
    D3D12_HEAP_TYPE_DEFAULT};
constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesUpload = {
    D3D12_HEAP_TYPE_UPLOAD};
constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesReadback = {
    D3D12_HEAP_TYPE_READBACK};

ID3D12RootSignature* CreateRootSignature(
    const D3D12Provider& provider, const D3D12_ROOT_SIGNATURE_DESC& desc) {
  ID3DBlob* blob;
  ID3DBlob* error_blob = nullptr;
  if (FAILED(provider.SerializeRootSignature(
          &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error_blob))) {
    XELOGE("Failed to serialize a root signature");
    if (error_blob != nullptr) {
      XELOGE("{}",
             reinterpret_cast<const char*>(error_blob->GetBufferPointer()));
      error_blob->Release();
    }
    return nullptr;
  }
  if (error_blob != nullptr) {
    error_blob->Release();
  }
  ID3D12RootSignature* root_signature = nullptr;
  provider.GetDevice()->CreateRootSignature(0, blob->GetBufferPointer(),
                                            blob->GetBufferSize(),
                                            IID_PPV_ARGS(&root_signature));
  blob->Release();
  return root_signature;
}

ID3D12PipelineState* CreateComputePipeline(
    ID3D12Device* device, const void* shader, size_t shader_size,
    ID3D12RootSignature* root_signature) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
  desc.pRootSignature = root_signature;
  desc.CS.pShaderBytecode = shader;
  desc.CS.BytecodeLength = shader_size;
  desc.NodeMask = 0;
  desc.CachedPSO.pCachedBlob = nullptr;
  desc.CachedPSO.CachedBlobSizeInBytes = 0;
  desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
  ID3D12PipelineState* pipeline = nullptr;
  // Game Vault 1.1.8.519: start-up black screen: how long the driver takes for each built-in compute pipeline.
  const auto gv_t0 = std::chrono::steady_clock::now();
  device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
  const uint64_t gv_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - gv_t0).count());
  static std::atomic<uint64_t> gv_count{0}, gv_total_us{0};
  const uint64_t n = ++gv_count, total = gv_total_us += gv_us;
  if (gv_us >= 50000 || (n % 25) == 0) {
    XELOGI("Game Vault: built-in compute pipeline #{} ({} bytes) took {} ms, {} ms for all so far", n, shader_size,
           gv_us / 1000, total / 1000);
  }
  return pipeline;
}

void CreateComputePipelinesParallel(ID3D12Device* device, const ComputePipelineJob* jobs, size_t count) {
  if (!count) {
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  // Biggest first, so the slowest builds don't start last.
  std::vector<size_t> order(count);
  for (size_t i = 0; i < count; ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [jobs](size_t a, size_t b) { return jobs[a].shader_size > jobs[b].shader_size; });
  std::atomic<size_t> next{0};
  auto work = [&]() {
    for (size_t k; (k = next.fetch_add(1)) < count;) {
      const ComputePipelineJob& job = jobs[order[k]];
      *job.out = CreateComputePipeline(device, job.shader, job.shader_size, job.root_signature);
    }
  };
  const size_t thread_count = std::min<size_t>(count, 6) - 1;  // + this thread
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (size_t i = 0; i < thread_count; ++i) threads.emplace_back(work);
  work();
  for (auto& t : threads) t.join();
  XELOGI("Game Vault: built {} built-in compute pipelines on {} threads in {} ms", count, thread_count + 1,
         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

void CreateBufferRawSRV(ID3D12Device* device,
                        D3D12_CPU_DESCRIPTOR_HANDLE handle,
                        ID3D12Resource* buffer, uint32_t size,
                        uint64_t offset) {
  assert_false(size & (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1));
  assert_false(offset & (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1));
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.FirstElement = offset >> 2;
  desc.Buffer.NumElements = size >> 2;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device->CreateShaderResourceView(buffer, &desc, handle);
}

void CreateBufferRawUAV(ID3D12Device* device,
                        D3D12_CPU_DESCRIPTOR_HANDLE handle,
                        ID3D12Resource* buffer, uint32_t size,
                        uint64_t offset) {
  assert_false(size & (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1));
  assert_false(offset & (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1));
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = offset >> 2;
  desc.Buffer.NumElements = size >> 2;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.CounterOffsetInBytes = 0;
  desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(buffer, nullptr, &desc, handle);
}

void CreateBufferTypedSRV(ID3D12Device* device,
                          D3D12_CPU_DESCRIPTOR_HANDLE handle,
                          ID3D12Resource* buffer, DXGI_FORMAT format,
                          uint32_t num_elements, uint64_t first_element) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.FirstElement = first_element;
  desc.Buffer.NumElements = num_elements;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  device->CreateShaderResourceView(buffer, &desc, handle);
}

void CreateBufferTypedUAV(ID3D12Device* device,
                          D3D12_CPU_DESCRIPTOR_HANDLE handle,
                          ID3D12Resource* buffer, DXGI_FORMAT format,
                          uint32_t num_elements, uint64_t first_element) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
  desc.Format = format;
  desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = first_element;
  desc.Buffer.NumElements = num_elements;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.CounterOffsetInBytes = 0;
  desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
  device->CreateUnorderedAccessView(buffer, nullptr, &desc, handle);
}

}  // namespace util
}  // namespace d3d12
}  // namespace ui
}  // namespace xe
