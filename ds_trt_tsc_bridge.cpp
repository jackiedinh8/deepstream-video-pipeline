#include <torch/script.h>
#include <torchvision/ops/nms.h>
#include <c10/cuda/CUDAGuard.h>
#include <nvToolsExt.h>

#include "nvdsinfer.h"
#include "nvdsinfer_custom_impl.h"

#include <iostream>
#include <memory>
#include <chrono>


const size_t outputLayerIndex = 0;
const uint32_t fpsFramePeriod = 128;


size_t ppmSave(const at::Tensor& image_chw, const std::string& filename) {
    FILE* outfile = fopen(filename.c_str(), "w");
    at::Tensor bytes_hwc = image_chw.permute({1, 2, 0}).to(torch::kCPU, torch::kUInt8, false, true).contiguous();
    unsigned int height = bytes_hwc.sizes()[0], width = bytes_hwc.sizes()[1];
    size_t n = 0;
    n += fprintf(outfile, "P6\n# THIS IS A COMMENT\n%d %d\n%d\n", width, height, 0xFF);
    n += fwrite((uint8_t*)bytes_hwc.data_ptr(), 1, width * height * 3, outfile);
    fclose(outfile);
    return n;
}

std::vector<std::string> split(const std::string& s, char delimiter) {
   std::vector<std::string> tokens;
   std::string token;
   std::istringstream tokenStream(s);
   while (std::getline(tokenStream, token, delimiter)) {
      tokens.push_back(token);
   }
   return tokens;
}

torch::ScalarType toTorchType(NvDsInferDataType nvdsType) {
    if(nvdsType == NvDsInferDataType::HALF) {
        return torch::kFloat16;
    } else if(nvdsType == NvDsInferDataType::INT8) {
        return torch::kUInt8;
    }
    return torch::kFloat32;
}

struct DeviceState {
    bool hasDummyDim = false;
    uint32_t bufferCount = 0;
    uint32_t profileFrameCount = 0;
    uint32_t detections = 0;
    std::chrono::time_point<std::chrono::system_clock> start = std::chrono::system_clock::now();
    torch::Device sourceDevice = torch::kCPU;
    torch::Device device = torch::kCPU;
    at::cuda::CUDAStream stream = at::cuda::getStreamFromPool();
    torch::jit::script::Module model;

    std::vector<uint32_t> inputLayerIndexes;
    std::vector<torch::ScalarType> inputLayerTypes;
};



bool DsTrtTscPostProcess(
    DeviceState &state,
    std::vector<NvDsInferLayerInfo> const &batchOutputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferObjectDetectionInfo> &batchObjectList
) {
    /* std::cout << state.bufferCount << "]]\n"; */

    at::cuda::CUDAStreamGuard streamGuard(state.stream);
    nvtxRangePushA("setup");

    unsigned int batchDim = 1;
    if(state.hasDummyDim) {
        batchDim = batchOutputLayersInfo[0].inferDims.d[0];
    }

    std::vector<torch::jit::IValue> inputTensors;

    for(uint32_t inputIndex = 0; inputIndex < state.inputLayerIndexes.size(); ++inputIndex) {
        auto& layer = batchOutputLayersInfo[state.inputLayerIndexes[inputIndex]];

        std::vector<int64_t> dims;
        for(unsigned int d = 0; d < layer.inferDims.numDims; ++d) dims.push_back(layer.inferDims.d[d]);

/*         if(dims[0] != batchDim) { */
/*             nvtxRangePop(); // setup */
/*             // torchscript export cannot handle dynamic batch dimension just yet */
/*             std::cout << dims[0] << " != " << batchDim << "\n"; */
/*             state.bufferCount += 1; */
/*             return true; */
/*         } */

        at::Tensor source_nchw = torch::from_blob(
            layer.buffer,
            c10::IntArrayRef(dims),
            torch::dtype(state.inputLayerTypes[inputIndex]).device(state.sourceDevice)
        );

        inputTensors.push_back(
            source_nchw.to(
                state.device,
                torch::kFloat32,
                /*non-blocking=*/true,
                /*copy=*/true
            ).contiguous()
        );
    }

    nvtxRangePop(); // setup

/*         if(state.bufferCount == (state.device.index() * 5)) { */
/*             at::Tensor batch_cpu = batch_nchw.to(torch::kCPU); */
/*             /1* std::cout << "batch bytes min/max/mean:\n" << batch_cpu.min() << "\n" << batch_cpu.max() << "\n" << batch_cpu.to(torch::kFloat32).mean() << "\n"; *1/ */
/*             /1* std::cout << batch_cpu.sizes() << "\n"; *1/ */
/*             for(unsigned int i = 0; i < batch_cpu.sizes()[0]; ++i) { */
/*                 ppmSave(batch_cpu.slice(0, i, i + 1).squeeze(0), "logs/test_" + std::to_string(state.bufferCount) + "_" + std::to_string(i) + ".ppm"); */
/*             } */
/*         } */

    nvtxRangePushA("inference");
    auto result = state.model.forward(inputTensors).toTuple();
    nvtxRangePop(); // inference

    state.detections += result->elements()[0].toTensor().sizes()[0];
    /* std::cout << state.device.index() << ":\t" << state.bufferCount << "]:\tdetections: " << << "\n"; */

    if(state.profileFrameCount >= fpsFramePeriod) {
        std::chrono::duration<double> elapsed_s = std::chrono::system_clock::now() - state.start;
        std::cout << "gpuId: " << state.device.index() << "\tbufferCount: " << std::setw(4) << state.bufferCount << "\tframes: " << state.profileFrameCount << "\tdetections: " << std::setw(5) << state.detections << "\tfps: " << state.profileFrameCount / elapsed_s.count() << "\n";
        state.profileFrameCount = 0;
        state.detections = 0;
        state.start = std::chrono::system_clock::now();
    }

    state.bufferCount += 1;
    state.profileFrameCount += batchDim;
    return true;
}



extern "C"
bool DsTrtTscBridgeHost(
    std::vector<NvDsInferLayerInfo> const &batchOutputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferObjectDetectionInfo> &batchObjectList
) {
    const uint32_t gpuId = 0;
    static DeviceState state;

    if(state.device == torch::Device(torch::kCPU)) {
        state.device = torch::Device(torch::kCUDA, gpuId);
        state.sourceDevice = torch::kCPU;
        state.stream = at::cuda::getStreamFromPool(true, gpuId);

        state.hasDummyDim = false;

        std::string modelPath(std::getenv("DS_TSC_PTH_PATH") + std::to_string(gpuId));
        std::cout << "Loading downstream model from " << modelPath << std::endl;
        state.model = torch::jit::load(modelPath);
        state.model.to(state.device);

        std::vector<std::string> inputLayerNames(split(std::getenv("DS_TSC_INPUTS"), ','));

        for(auto inputName = inputLayerNames.begin(); inputName != inputLayerNames.end(); ++inputName) {
            for(uint32_t layerIndex = 0; layerIndex < batchOutputLayersInfo.size(); ++layerIndex) {
                if(*inputName == batchOutputLayersInfo[layerIndex].layerName) {
                    state.inputLayerIndexes.push_back(layerIndex);
                    state.inputLayerTypes.push_back(toTorchType(batchOutputLayersInfo[layerIndex].dataType));
                }
            }
        }

        state.start = std::chrono::system_clock::now();
    }

    return DsTrtTscPostProcess(
        state,
        batchOutputLayersInfo,
        networkInfo,
        detectionParams,
        batchObjectList
    );
}

extern "C"
bool DsTrtTscBridgeDevice(
    uint32_t gpuId,
    std::vector<NvDsInferLayerInfo> const &batchOutputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferObjectDetectionInfo> &batchObjectList
) {
    static std::vector<DeviceState> deviceState(4);

    DeviceState& state = deviceState[gpuId];

    if(state.device == torch::Device(torch::kCPU)) {
        state.device = torch::Device(torch::kCUDA, gpuId);
        state.sourceDevice = state.device;
        state.stream = at::cuda::getStreamFromPool(true, gpuId);

        state.hasDummyDim = true;

        std::string modelPath(std::getenv("DS_TSC_PTH_PATH") + std::to_string(gpuId));
        std::cout << "Loading downstream model from " << modelPath << std::endl;
        state.model = torch::jit::load(modelPath);
        state.model.to(state.device);

        std::vector<std::string> inputLayerNames(split(std::getenv("DS_TSC_INPUTS"), ','));

        for(auto inputName = inputLayerNames.begin(); inputName != inputLayerNames.end(); ++inputName) {
            for(uint32_t layerIndex = 0; layerIndex < batchOutputLayersInfo.size(); ++layerIndex) {
                if(*inputName == batchOutputLayersInfo[layerIndex].layerName) {
                    state.inputLayerIndexes.push_back(layerIndex);
                    state.inputLayerTypes.push_back(toTorchType(batchOutputLayersInfo[layerIndex].dataType));
                }
            }
        }

        state.start = std::chrono::system_clock::now();
    }

    return DsTrtTscPostProcess(
        state,
        batchOutputLayersInfo,
        networkInfo,
        detectionParams,
        batchObjectList
    );
}



