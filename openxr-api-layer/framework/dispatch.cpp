// MIT License
//
// Copyright(c) 2021-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include <layer.h>

#include "dispatch.h"
#include "log.h"

#define THISLAYER_VERSION 1
#define THISLAYER_DESC "An API layer template"

using namespace openxr_api_layer::log;

namespace openxr_api_layer {

    // Request the layer 'name' for the extensions it provides and append these to the complete list
    void appendAvailableExtensions(const char * name, PFN_xrGetInstanceProcAddr getInstanceProcAddr, std::unordered_map<std::string, uint32_t> &availableExtensions)
    {
        XrResult stillOK{XR_SUCCESS};
        PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties;
        stillOK = getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
            reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties));
        uint32_t extCount{0};
        stillOK = XR_SUCCEEDED(stillOK) ? xrEnumerateInstanceExtensionProperties(name, 0, &extCount, nullptr) : stillOK;
        std::vector<XrExtensionProperties> extensions(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
        stillOK = XR_SUCCEEDED(stillOK) ? xrEnumerateInstanceExtensionProperties(name, extCount, &extCount, extensions.data()) : stillOK;
        for (const auto& ext: extensions) {
            // TODO: For now the extensions are added to the list only they are not already in it
            // It may be interesting to check for the versions and add the highest one
            availableExtensions.emplace(ext.extensionName, ext.extensionVersion);
        }
    }

    // Entry point for creating the layer.
    XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* const instanceCreateInfo,
                                                 const struct XrApiLayerCreateInfo* const apiLayerInfo,
                                                 XrInstance* const instance) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrCreateApiLayerInstance");

        if (!apiLayerInfo || apiLayerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
            apiLayerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
            apiLayerInfo->structSize != sizeof(XrApiLayerCreateInfo) || !apiLayerInfo->nextInfo ||
            apiLayerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
            apiLayerInfo->nextInfo->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
            apiLayerInfo->nextInfo->structSize != sizeof(XrApiLayerNextInfo) || !apiLayerInfo->nextInfo->layerName ||
            std::string_view(apiLayerInfo->nextInfo->layerName) != LAYER_NAME ||
            !apiLayerInfo->nextInfo->nextGetInstanceProcAddr || !apiLayerInfo->nextInfo->nextCreateApiLayerInstance) {
            ErrorLog("xrCreateApiLayerInstance validation failed\n");
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        // Dump the other layers.
        {
            auto info = apiLayerInfo->nextInfo;
            while (info) {
                TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance", TLArg(info->layerName, "LayerName"));
                Log(fmt::format("Using layer: {}\n", info->layerName));
                info = info->next;
            }
        }

        // Only request implicit extensions that are supported.
        //
        // Layers only need to report the extensions they support so ask each layer as well as the runtime for the complete list
        std::vector<std::string> filteredImplicitExtensions;
        if (!implicitExtensions.empty()) {
            std::unordered_map<std::string, uint32_t> availableExtensions{};
            auto info = apiLayerInfo->nextInfo;
            // Query the extensions properties for each layer in the chain
            while (info->next != nullptr) {
                appendAvailableExtensions(info->next->layerName, info->nextGetInstanceProcAddr, availableExtensions);
                info = info->next;
            }
            // Query the extensions properties of the runtime too 
            appendAvailableExtensions(nullptr, info->nextGetInstanceProcAddr, availableExtensions);

            for (const std::string& extensionName: implicitExtensions) {
                // TODO: it may also be interesting to check the extension version
                if (auto search = availableExtensions.find(extensionName); search != availableExtensions.end()) {
                    filteredImplicitExtensions.push_back(extensionName);
                } else {
                    Log(fmt::format("Cannot satisfy implicit extension request: {}\n", extensionName));
                    return XR_ERROR_EXTENSION_NOT_PRESENT;
                }
            }
        }

        // Dump the requested extensions.
        XrInstanceCreateInfo chainInstanceCreateInfo = *instanceCreateInfo;
        std::vector<const char*> newEnabledExtensionNames;
        for (uint32_t i = 0; i < chainInstanceCreateInfo.enabledExtensionCount; i++) {
            const std::string_view ext(chainInstanceCreateInfo.enabledExtensionNames[i]);
            TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance", TLArg(ext.data(), "ExtensionName"));

            if (std::find(blockedExtensions.cbegin(), blockedExtensions.cend(), ext) == blockedExtensions.cend()) {
                Log(fmt::format("Requested extension: {}\n", ext));
                newEnabledExtensionNames.push_back(ext.data());
            } else {
                Log(fmt::format("Blocking extension: {}\n", ext));
            }
        }
        for (const auto& ext : filteredImplicitExtensions) {
            Log(fmt::format("Requesting extension: {}\n", ext));
            newEnabledExtensionNames.push_back(ext.c_str());
        }
        chainInstanceCreateInfo.enabledExtensionNames = newEnabledExtensionNames.data();
        chainInstanceCreateInfo.enabledExtensionCount = (uint32_t)newEnabledExtensionNames.size();

        // Call the chain to create the instance.
        XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
        chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;
        XrResult result =
            apiLayerInfo->nextInfo->nextCreateApiLayerInstance(&chainInstanceCreateInfo, &chainApiLayerInfo, instance);
        if (result == XR_SUCCESS) {
            // Create our layer.
            openxr_api_layer::GetInstance()->SetGetInstanceProcAddr(apiLayerInfo->nextInfo->nextGetInstanceProcAddr,
                                                                    *instance);
            openxr_api_layer::GetInstance()->SetGrantedExtensions(filteredImplicitExtensions);

            // Forward the xrCreateInstance() call to the layer.
            try {
                result = openxr_api_layer::GetInstance()->xrCreateInstance(instanceCreateInfo);
            } catch (std::exception& exc) {
                TraceLoggingWriteTagged(local, "xrCreateInstance_Error", TLArg(exc.what(), "Error"));
                ErrorLog(fmt::format("xrCreateInstance: {}\n", exc.what()));
                result = XR_ERROR_RUNTIME_FAILURE;
            }

            // Cleanup attempt before returning an error.
            if (XR_FAILED(result)) {
                PFN_xrDestroyInstance xrDestroyInstance = nullptr;
                if (XR_SUCCEEDED(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                        *instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyInstance)))) {
                    xrDestroyInstance(*instance);
                }
            }
        }

        TraceLoggingWriteStop(local, "xrCreateApiLayerInstance", TLArg(xr::ToCString(result), "Result"));
        if (XR_FAILED(result)) {
            ErrorLog(fmt::format("xrCreateApiLayerInstance failed with {}\n", xr::ToCString(result)));
        }

        return result;
    }

    // Report this layer extensions properties
    XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t propertyCapacityInput, uint32_t* propertyCountOutput, XrExtensionProperties* properties)
    {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrEnumerateInstanceExtensionProperties");

        XrResult result{XR_SUCCESS};
        try
        {
            if (nullptr == propertyCountOutput || !layerName || std::string_view(layerName) != LAYER_NAME) {
                result = XR_ERROR_VALIDATION_FAILURE;
            } else {
                uint32_t num_extension_properties = static_cast<uint32_t>(advertisedExtensions.size());
                if (propertyCapacityInput == 0) {
                    *propertyCountOutput = num_extension_properties;
                } else if (nullptr != properties) {
                    if (propertyCapacityInput < num_extension_properties) {
                        result = XR_ERROR_SIZE_INSUFFICIENT;
                    } else {
                        // All parameters are valid, proceed to fill the data
                        uint32_t idx = 0;
                        for (const auto [extName, extVersion] : advertisedExtensions) {
                            XrExtensionProperties extProperty{XR_TYPE_EXTENSION_PROPERTIES};
                            strcpy(extProperty.extensionName, extName.c_str());
                            extProperty.extensionVersion = extVersion;
                            properties[idx] = extProperty;
                            idx++;
                        }
                        *propertyCountOutput = num_extension_properties;
                    }
                } else {
                    result = XR_ERROR_VALIDATION_FAILURE;
                }
            }
        }
        catch (std::exception& exc)
        {
            TraceLoggingWriteTagged(local, "xrEnumerateInstanceExtensionProperties_Error", TLArg(exc.what(), "Error"));
            ErrorLog(fmt::format("xrEnumerateInstanceExtensionProperties: {}\n", exc.what()));
            result = XR_ERROR_RUNTIME_FAILURE;
        }

        TraceLoggingWriteStop(local, "xrEnumerateInstanceExtensionProperties", TLArg(xr::ToCString(result), "Result"));
        if (XR_FAILED(result)) {
            ErrorLog(fmt::format("xrEnumerateInstanceExtensionProperties failed with {}\n", xr::ToCString(result)));
        }

        return result;
    }

    // Report this layer properties
    XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput, uint32_t *propertyCountOutput, XrApiLayerProperties *properties)
    {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrEnumerateApiLayerProperties");

        XrResult result{XR_SUCCESS};
        try
        {
            if (nullptr == propertyCountOutput) {
                result = XR_ERROR_VALIDATION_FAILURE;
            } else {
                uint32_t num_layer_properties{1};
                if (propertyCapacityInput == 0) {
                    *propertyCountOutput = num_layer_properties;
                } else if (nullptr != properties) {
                    if (propertyCapacityInput < num_layer_properties) {
                        return XR_ERROR_SIZE_INSUFFICIENT;
                    } else {
                        // All parameters are valid, proceed to fill the data
                        XrApiLayerProperties layerProp{XR_TYPE_API_LAYER_PROPERTIES};
                        layerProp.next = nullptr;
                        strncpy(layerProp.layerName, LAYER_NAME, XR_MAX_API_LAYER_NAME_SIZE - 1);
                        if (std::string_view(LAYER_NAME).size() >= XR_MAX_API_LAYER_NAME_SIZE - 1) {
                            layerProp.layerName[XR_MAX_API_LAYER_NAME_SIZE - 1] = '\0';
                        }
                        // TODO: define THISLAYER_DESC and THISLAYER_VERSION based on the manifest.json
                        strncpy(layerProp.description, THISLAYER_DESC, XR_MAX_API_LAYER_DESCRIPTION_SIZE - 1);
                        if (std::string_view(THISLAYER_DESC).size() >= XR_MAX_API_LAYER_DESCRIPTION_SIZE - 1) {
                            layerProp.description[XR_MAX_API_LAYER_DESCRIPTION_SIZE - 1] = '\0';
                        }
                        layerProp.layerVersion = THISLAYER_VERSION;
                        layerProp.specVersion = XR_MAKE_VERSION(1, 0, XR_VERSION_PATCH(XR_CURRENT_API_VERSION));
                        properties[0] = layerProp;

                        *propertyCountOutput = num_layer_properties;
                    }
                } else {
                    result = XR_ERROR_VALIDATION_FAILURE;
                }
            }
        }
        catch (std::exception& exc)
        {
            TraceLoggingWriteTagged(local, "xrEnumerateApiLayerProperties_Error", TLArg(exc.what(), "Error"));
            ErrorLog(fmt::format("xrEnumerateApiLayerProperties: {}\n", exc.what()));
            result = XR_ERROR_RUNTIME_FAILURE;
        }

        TraceLoggingWriteStop(local, "xrEnumerateApiLayerProperties", TLArg(xr::ToCString(result), "Result"));
        if (XR_FAILED(result)) {
            ErrorLog(fmt::format("xrEnumerateApiLayerProperties failed with {}\n", xr::ToCString(result)));
        }

        return result;
    }

    // Forward the xrGetInstanceProcAddr() call to the dispatcher.
    XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrGetInstanceProcAddr");

        XrResult result{XR_ERROR_FUNCTION_UNSUPPORTED};
        try {
            if (name && std::string_view(name) == "xrEnumerateInstanceExtensionProperties") {
                // Always call the layer implementation of xrEnumerateInstanceExtensionProperties
                *function = reinterpret_cast<PFN_xrVoidFunction>(openxr_api_layer::xrEnumerateInstanceExtensionProperties);
                result = XR_SUCCESS;
            } else if (name && std::string_view(name) == "xrEnumerateApiLayerProperties") {
                // Always call the layer implementation of xrEnumerateApiLayerProperties
                *function = reinterpret_cast<PFN_xrVoidFunction>(openxr_api_layer::xrEnumerateApiLayerProperties);
                result = XR_SUCCESS;
            } else if (instance != XR_NULL_HANDLE && name != nullptr) {
                result = openxr_api_layer::GetInstance()->xrGetInstanceProcAddr(instance, name, function);
            }
        } catch (std::exception& exc) {
            TraceLoggingWriteTagged(local, "xrGetInstanceProcAddr_Error", TLArg(exc.what(), "Error"));
            ErrorLog(fmt::format("xrGetInstanceProcAddr: {}\n", exc.what()));
            result = XR_ERROR_RUNTIME_FAILURE;
        }

        TraceLoggingWriteStop(local, "xrGetInstanceProcAddr", TLArg(xr::ToCString(result), "Result"));

        return result;
    }

} // namespace openxr_api_layer
