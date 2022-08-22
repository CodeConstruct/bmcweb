/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#pragma once

#include "dbus_singleton.hpp"
#include "error_messages.hpp"
#include "health.hpp"

#include <app.hpp>
#include <boost/container/flat_map.hpp>
#include <dbus_utility.hpp>
#include <query.hpp>
#include <registries/privilege_registry.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/message/native_types.hpp>
#include <sdbusplus/unpack_properties.hpp>
#include <sdbusplus/utility/dedup_variant.hpp>
#include <utils/collection.hpp>
#include <utils/dbus_utils.hpp>
#include <utils/json_utils.hpp>
#include <utils/location_utils.hpp>
#include <utils/log_utils.hpp>

#include <algorithm>
#include <unordered_map>

namespace redfish
{

using resourceIdToSubtreeRespMapType =
    std::unordered_map<std::string,
                       std::pair<std::string, dbus::utility::MapperServiceMap>>;

inline void getSubProcessorThreadCollectionWithExpand(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const nlohmann::json::json_pointer& jsonPtr, uint8_t expandLevel,
    const std::string& processorId, const std::string& coreId,
    const std::string& corePath);

inline void getSubProcessorCoreCollectionWithExpand(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const nlohmann::json::json_pointer& jsonPtr, uint8_t expandLevel,
    const std::string& processorId, const std::string& cpuPath);

inline void getProcessorCollectionWithExpand(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp, uint8_t expandLevel);

// Interfaces which imply a D-Bus object represents a Processor
constexpr std::array<const char*, 2> processorInterfaces = {
    "xyz.openbmc_project.Inventory.Item.Cpu",
    "xyz.openbmc_project.Inventory.Item.Accelerator"};

/**
 * @brief Fill out uuid info of a processor by
 * requesting data from the given D-Bus object.
 *
 * @param[in,out]   aResp       Async HTTP response.
 * @param[in]       service     D-Bus service to query.
 * @param[in]       objPath     D-Bus object to query.
 * @param[in]       jsonPtr     json pointer to index the response fields
 */
inline void getProcessorUUID(std::shared_ptr<bmcweb::AsyncResp> aResp,
                             const std::string& service,
                             const std::string& objPath,
                             const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG << "Get Processor UUID";
    sdbusplus::asio::getProperty<std::string>(
        *crow::connections::systemBus, service, objPath,
        "xyz.openbmc_project.Common.UUID", "UUID",
        [objPath, jsonPtr, aResp{std::move(aResp)}](
            const boost::system::error_code ec, const std::string& property) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error";
            messages::internalError(aResp->res);
            return;
        }
        aResp->res.jsonValue[jsonPtr]["UUID"] = property;
        });
}

inline void getCpuDataByInterface(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const nlohmann::json::json_pointer& jsonPtr,
    const dbus::utility::DBusInteracesMap& cpuInterfacesProperties)
{
    BMCWEB_LOG_DEBUG << "Get CPU resources by interface.";

    // Set the default value of state
    aResp->res.jsonValue[jsonPtr]["Status"]["State"] = "Enabled";
    aResp->res.jsonValue[jsonPtr]["Status"]["Health"] = "OK";

    for (const auto& interface : cpuInterfacesProperties)
    {
        for (const auto& property : interface.second)
        {
            if (property.first == "Present")
            {
                const bool* cpuPresent = std::get_if<bool>(&property.second);
                if (cpuPresent == nullptr)
                {
                    // Important property not in desired type
                    messages::internalError(aResp->res);
                    return;
                }
                if (!*cpuPresent)
                {
                    // Slot is not populated
                    aResp->res.jsonValue[jsonPtr]["Status"]["State"] = "Absent";
                }
            }
            else if (property.first == "Functional")
            {
                const bool* cpuFunctional = std::get_if<bool>(&property.second);
                if (cpuFunctional == nullptr)
                {
                    messages::internalError(aResp->res);
                    return;
                }
                if (!*cpuFunctional)
                {
                    aResp->res.jsonValue[jsonPtr]["Status"]["Health"] =
                        "Critical";
                }
            }
            else if (property.first == "CoreCount")
            {
                const uint16_t* coresCount =
                    std::get_if<uint16_t>(&property.second);
                if (coresCount == nullptr)
                {
                    messages::internalError(aResp->res);
                    return;
                }
                aResp->res.jsonValue[jsonPtr]["TotalCores"] = *coresCount;
            }
            else if (property.first == "MaxSpeedInMhz")
            {
                const uint32_t* value = std::get_if<uint32_t>(&property.second);
                if (value != nullptr)
                {
                    aResp->res.jsonValue[jsonPtr]["MaxSpeedMHz"] = *value;
                }
            }
            else if (property.first == "Socket")
            {
                const std::string* value =
                    std::get_if<std::string>(&property.second);
                if (value != nullptr)
                {
                    aResp->res.jsonValue[jsonPtr]["Socket"] = *value;
                }
            }
            else if (property.first == "ThreadCount")
            {
                const uint16_t* value = std::get_if<uint16_t>(&property.second);
                if (value != nullptr)
                {
                    aResp->res.jsonValue[jsonPtr]["TotalThreads"] = *value;
                }
            }
            else if (property.first == "EffectiveFamily")
            {
                const uint16_t* value = std::get_if<uint16_t>(&property.second);
                if (value != nullptr && *value != 2)
                {
                    aResp->res
                        .jsonValue[jsonPtr]["ProcessorId"]["EffectiveFamily"] =
                        "0x" + intToHexString(*value, 4);
                }
            }
            else if (property.first == "EffectiveModel")
            {
                const uint16_t* value = std::get_if<uint16_t>(&property.second);
                if (value == nullptr)
                {
                    messages::internalError(aResp->res);
                    return;
                }
                if (*value != 0)
                {
                    aResp->res
                        .jsonValue[jsonPtr]["ProcessorId"]["EffectiveModel"] =
                        "0x" + intToHexString(*value, 4);
                }
            }
            else if (property.first == "Id")
            {
                const uint64_t* value = std::get_if<uint64_t>(&property.second);
                if (value != nullptr && *value != 0)
                {
                    aResp->res.jsonValue[jsonPtr]["ProcessorId"]
                                        ["IdentificationRegisters"] =
                        "0x" + intToHexString(*value, 16);
                }
            }
            else if (property.first == "Microcode")
            {
                const uint32_t* value = std::get_if<uint32_t>(&property.second);
                if (value == nullptr)
                {
                    messages::internalError(aResp->res);
                    return;
                }
                if (*value != 0)
                {
                    aResp->res
                        .jsonValue[jsonPtr]["ProcessorId"]["MicrocodeInfo"] =
                        "0x" + intToHexString(*value, 8);
                }
            }
            else if (property.first == "Step")
            {
                const uint16_t* value = std::get_if<uint16_t>(&property.second);
                if (value == nullptr)
                {
                    messages::internalError(aResp->res);
                    return;
                }
                if (*value != 0)
                {
                    aResp->res.jsonValue[jsonPtr]["ProcessorId"]["Step"] =
                        "0x" + intToHexString(*value, 4);
                }
            }
        }
    }
}

inline void getCpuDataByService(std::shared_ptr<bmcweb::AsyncResp> aResp,
                                const std::string& cpuId,
                                const std::string& service,
                                const std::string& objPath,
                                const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG << "Get available system cpu resources by service.";

    crow::connections::systemBus->async_method_call(
        [cpuId, service, objPath, jsonPtr, aResp{std::move(aResp)}](
            const boost::system::error_code ec,
            const dbus::utility::ManagedObjectType& dbusData) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error";
            messages::internalError(aResp->res);
            return;
        }
        aResp->res.jsonValue[jsonPtr]["Id"] = cpuId;
        aResp->res.jsonValue[jsonPtr]["Name"] = "Processor";
        aResp->res.jsonValue[jsonPtr]["ProcessorType"] = "CPU";

        bool slotPresent = false;
        std::string corePath = objPath + "/core";
        size_t totalCores = 0;
        for (const auto& object : dbusData)
        {
            if (object.first.str == objPath)
            {
                getCpuDataByInterface(aResp, jsonPtr, object.second);
            }
            else if (object.first.str.starts_with(corePath))
            {
                for (const auto& interface : object.second)
                {
                    if (interface.first == "xyz.openbmc_project.Inventory.Item")
                    {
                        for (const auto& property : interface.second)
                        {
                            if (property.first == "Present")
                            {
                                const bool* present =
                                    std::get_if<bool>(&property.second);
                                if (present != nullptr)
                                {
                                    if (*present)
                                    {
                                        slotPresent = true;
                                        totalCores++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // In getCpuDataByInterface(), state and health are set
        // based on the present and functional status. If core
        // count is zero, then it has a higher precedence.
        if (slotPresent)
        {
            if (totalCores == 0)
            {
                // Slot is not populated, set status end return
                aResp->res.jsonValue[jsonPtr]["Status"]["State"] = "Absent";
                aResp->res.jsonValue[jsonPtr]["Status"]["Health"] = "OK";
            }
            aResp->res.jsonValue[jsonPtr]["TotalCores"] = totalCores;
        }
        return;
        },
        service, "/xyz/openbmc_project/inventory",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

inline void getCpuAssetData(std::shared_ptr<bmcweb::AsyncResp> aResp,
                            const std::string& service,
                            const std::string& objPath,
                            const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG << "Get Cpu Asset Data";
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, service, objPath,
        "xyz.openbmc_project.Inventory.Decorator.Asset",
        [objPath, jsonPtr, aResp{std::move(aResp)}](
            const boost::system::error_code ec,
            const dbus::utility::DBusPropertiesMap& properties) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error";
            messages::internalError(aResp->res);
            return;
        }

        const std::string* serialNumber = nullptr;
        const std::string* model = nullptr;
        const std::string* manufacturer = nullptr;
        const std::string* partNumber = nullptr;
        const std::string* sparePartNumber = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), properties, "SerialNumber",
            serialNumber, "Model", model, "Manufacturer", manufacturer,
            "PartNumber", partNumber, "SparePartNumber", sparePartNumber);

        if (!success)
        {
            messages::internalError(aResp->res);
            return;
        }

        if (serialNumber != nullptr && !serialNumber->empty())
        {
            aResp->res.jsonValue[jsonPtr]["SerialNumber"] = *serialNumber;
        }

        if ((model != nullptr) && !model->empty())
        {
            aResp->res.jsonValue[jsonPtr]["Model"] = *model;
        }

        if (manufacturer != nullptr)
        {
            aResp->res.jsonValue[jsonPtr]["Manufacturer"] = *manufacturer;

            // Otherwise would be unexpected.
            if (manufacturer->find("Intel") != std::string::npos)
            {
                aResp->res.jsonValue[jsonPtr]["ProcessorArchitecture"] = "x86";
                aResp->res.jsonValue[jsonPtr]["InstructionSet"] = "x86-64";
            }
            else if (manufacturer->find("IBM") != std::string::npos)
            {
                aResp->res.jsonValue[jsonPtr]["ProcessorArchitecture"] =
                    "Power";
                aResp->res.jsonValue[jsonPtr]["InstructionSet"] = "PowerISA";
            }
        }

        if (partNumber != nullptr)
        {
            aResp->res.jsonValue[jsonPtr]["PartNumber"] = *partNumber;
        }

        if (sparePartNumber != nullptr && !sparePartNumber->empty())
        {
            aResp->res.jsonValue[jsonPtr]["SparePartNumber"] = *sparePartNumber;
        }
        });
}

inline void getCpuRevisionData(std::shared_ptr<bmcweb::AsyncResp> aResp,
                               const std::string& service,
                               const std::string& objPath,
                               const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG << "Get Cpu Revision Data";
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, service, objPath,
        "xyz.openbmc_project.Inventory.Decorator.Revision",
        [objPath, jsonPtr, aResp{std::move(aResp)}](
            const boost::system::error_code ec,
            const dbus::utility::DBusPropertiesMap& properties) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error";
            messages::internalError(aResp->res);
            return;
        }

        const std::string* version = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), properties, "Version", version);

        if (!success)
        {
            messages::internalError(aResp->res);
            return;
        }

        if (version != nullptr)
        {
            aResp->res.jsonValue[jsonPtr]["Version"] = *version;
        }
        });
}

inline void getAcceleratorDataByService(
    std::shared_ptr<bmcweb::AsyncResp> aResp, const std::string& acclrtrId,
    const std::string& service, const std::string& objPath,
    const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG
        << "Get available system Accelerator resources by service.";
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, service, objPath, "",
        [acclrtrId, jsonPtr, aResp{std::move(aResp)}](
            const boost::system::error_code ec,
            const dbus::utility::DBusPropertiesMap& properties) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error";
            messages::internalError(aResp->res);
            return;
        }

        const bool* functional = nullptr;
        const bool* present = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), properties, "Functional",
            functional, "Present", present);

        if (!success)
        {
            messages::internalError(aResp->res);
            return;
        }

        std::string state = "Enabled";
        std::string health = "OK";

        if (present != nullptr && !*present)
        {
            state = "Absent";
        }

        if (functional != nullptr && !*functional)
        {
            if (state == "Enabled")
            {
                health = "Critical";
            }
        }

        aResp->res.jsonValue[jsonPtr]["Id"] = acclrtrId;
        aResp->res.jsonValue[jsonPtr]["Name"] = "Processor";
        aResp->res.jsonValue[jsonPtr]["Status"]["State"] = state;
        aResp->res.jsonValue[jsonPtr]["Status"]["Health"] = health;
        aResp->res.jsonValue[jsonPtr]["ProcessorType"] = "Accelerator";
        });
}

// OperatingConfig D-Bus Types
using TurboProfileProperty = std::vector<std::tuple<uint32_t, size_t>>;
using BaseSpeedPrioritySettingsProperty =
    std::vector<std::tuple<uint32_t, std::vector<uint32_t>>>;
// uint32_t and size_t may or may not be the same type, requiring a dedup'd
// variant

/**
 * Fill out the HighSpeedCoreIDs in a Processor resource from the given
 * OperatingConfig D-Bus property.
 *
 * @param[in,out]   aResp               Async HTTP response.
 * @param[in]       jsonPtr     json pointer to index the response fields
 * @param[in]       baseSpeedSettings   Full list of base speed priority groups,
 *                                      to use to determine the list of high
 *                                      speed cores.
 */
inline void highSpeedCoreIdsHandler(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const nlohmann::json::json_pointer& jsonPtr,
    const BaseSpeedPrioritySettingsProperty& baseSpeedSettings)
{
    // The D-Bus property does not indicate which bucket is the "high
    // priority" group, so let's discern that by looking for the one with
    // highest base frequency.
    auto highPriorityGroup = baseSpeedSettings.cend();
    uint32_t highestBaseSpeed = 0;
    for (auto it = baseSpeedSettings.cbegin(); it != baseSpeedSettings.cend();
         ++it)
    {
        const uint32_t baseFreq = std::get<uint32_t>(*it);
        if (baseFreq > highestBaseSpeed)
        {
            highestBaseSpeed = baseFreq;
            highPriorityGroup = it;
        }
    }

    nlohmann::json& jsonCoreIds =
        aResp->res.jsonValue[jsonPtr]["HighSpeedCoreIDs"];
    jsonCoreIds = nlohmann::json::array();

    // There may not be any entries in the D-Bus property, so only populate
    // if there was actually something there.
    if (highPriorityGroup != baseSpeedSettings.cend())
    {
        jsonCoreIds = std::get<std::vector<uint32_t>>(*highPriorityGroup);
    }
}

/**
 * Fill out OperatingConfig related items in a Processor resource by requesting
 * data from the given D-Bus object.
 *
 * @param[in,out]   aResp       Async HTTP response.
 * @param[in]       cpuId       CPU D-Bus name.
 * @param[in]       service     D-Bus service to query.
 * @param[in]       objPath     D-Bus object to query.
 * @param[in]       jsonPtr     json pointer to index the response fields
 */
inline void getCpuConfigData(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                             const std::string& cpuId,
                             const std::string& service,
                             const std::string& objPath,
                             const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_INFO << "Getting CPU operating configs for " << cpuId;

    // First, GetAll CurrentOperatingConfig properties on the object
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, service, objPath,
        "xyz.openbmc_project.Control.Processor.CurrentOperatingConfig",
        [aResp, jsonPtr, cpuId,
         service](const boost::system::error_code ec,
                  const dbus::utility::DBusPropertiesMap& properties) {
        if (ec)
        {
            BMCWEB_LOG_WARNING << "D-Bus error: " << ec << ", " << ec.message();
            messages::internalError(aResp->res);
            return;
        }

        nlohmann::json& json = aResp->res.jsonValue;

        const sdbusplus::message::object_path* appliedConfig = nullptr;
        const bool* baseSpeedPriorityEnabled = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), properties, "AppliedConfig",
            appliedConfig, "BaseSpeedPriorityEnabled",
            baseSpeedPriorityEnabled);

        if (!success)
        {
            messages::internalError(aResp->res);
            return;
        }

        if (appliedConfig != nullptr)
        {
            const std::string& dbusPath = appliedConfig->str;
            std::string uri = "/redfish/v1/Systems/system/Processors/" + cpuId +
                              "/OperatingConfigs";
            nlohmann::json::object_t operatingConfig;
            operatingConfig["@odata.id"] = uri;
            json[jsonPtr]["OperatingConfigs"] = std::move(operatingConfig);

            // Reuse the D-Bus config object name for the Redfish
            // URI
            size_t baseNamePos = dbusPath.rfind('/');
            if (baseNamePos == std::string::npos ||
                baseNamePos == (dbusPath.size() - 1))
            {
                // If the AppliedConfig was somehow not a valid path,
                // skip adding any more properties, since everything
                // else is tied to this applied config.
                messages::internalError(aResp->res);
                return;
            }
            uri += '/';
            uri += dbusPath.substr(baseNamePos + 1);
            nlohmann::json::object_t appliedOperatingConfig;
            appliedOperatingConfig["@odata.id"] = uri;
            json[jsonPtr]["AppliedOperatingConfig"] =
                std::move(appliedOperatingConfig);

            // Once we found the current applied config, queue another
            // request to read the base freq core ids out of that
            // config.
            sdbusplus::asio::getProperty<BaseSpeedPrioritySettingsProperty>(
                *crow::connections::systemBus, service, dbusPath,
                "xyz.openbmc_project.Inventory.Item.Cpu."
                "OperatingConfig",
                "BaseSpeedPrioritySettings",
                [aResp, jsonPtr](
                    const boost::system::error_code ec2,
                    const BaseSpeedPrioritySettingsProperty& baseSpeedList) {
                if (ec2)
                {
                    BMCWEB_LOG_WARNING << "D-Bus Property Get error: " << ec2;
                    messages::internalError(aResp->res);
                    return;
                }

                highSpeedCoreIdsHandler(aResp, jsonPtr, baseSpeedList);
                });
        }

        if (baseSpeedPriorityEnabled != nullptr)
        {
            json[jsonPtr]["BaseSpeedPriorityState"] =
                *baseSpeedPriorityEnabled ? "Enabled" : "Disabled";
        }
        });
}

/**
 * Populate the unique identifier in a Processor resource by requesting data
 * from the given D-Bus object.
 *
 * @param[in,out]   aResp       Async HTTP response.
 * @param[in]       service     D-Bus service to query.
 * @param[in]       objPath     D-Bus object to query.
 * @param[in]       jsonPtr     json pointer to index the response fields
 */
inline void getCpuUniqueId(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                           const std::string& service,
                           const std::string& objectPath,
                           const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG << "Get CPU UniqueIdentifier";
    sdbusplus::asio::getProperty<std::string>(
        *crow::connections::systemBus, service, objectPath,
        "xyz.openbmc_project.Inventory.Decorator.UniqueIdentifier",
        "UniqueIdentifier",
        [aResp, jsonPtr](boost::system::error_code ec, const std::string& id) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "Failed to read cpu unique id: " << ec;
            messages::internalError(aResp->res);
            return;
        }
        aResp->res.jsonValue[jsonPtr]["ProcessorId"]
                            ["ProtectedIdentificationNumber"] = id;
        });
}

inline void getCpuChassisAssociation(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& processorId, const std::string& objectPath,
    const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG << "Get CPU -- Chassis association";

    sdbusplus::asio::getProperty<std::vector<std::string>>(
        *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper",
        objectPath + "/chassis", "xyz.openbmc_project.Association", "endpoints",
        [asyncResp, jsonPtr,
         processorId](const boost::system::error_code ec,
                      const std::vector<std::string>& chassisList) {
        if (ec)
        {
            return;
        }
        if (chassisList.empty())
        {
            return;
        }
        if (chassisList.size() > 1)
        {
            BMCWEB_LOG_DEBUG << processorId
                             << " is associated with mutliple chassis";
            return;
        }

        sdbusplus::message::object_path chassisPath(chassisList[0]);
        std::string chassisName = chassisPath.filename();
        if (chassisName.empty())
        {
            BMCWEB_LOG_ERROR << "filename() is empty in " << chassisPath.str;
            return;
        }
        asyncResp->res.jsonValue[jsonPtr]["Links"]["Chassis"] = {
            {"@odata.id", "/redfish/v1/Chassis/" + chassisName}};
        });
}

/**
 * Find the D-Bus object representing the requested Processor, and call the
 * handler with the results. If matching object is not found, add 404 error to
 * response and don't call the handler.
 *
 * @param[in,out]   resp            Async HTTP response.
 * @param[in]       processorId     Redfish Processor Id.
 * @param[in]       handler         Callback to continue processing request upon
 *                                  successfully finding object.
 */
template <typename Handler>
inline void getProcessorObject(const std::shared_ptr<bmcweb::AsyncResp>& resp,
                               const std::string& processorId,
                               Handler&& handler)
{
    BMCWEB_LOG_DEBUG << "Get available system processor resources.";

    // GetSubTree on all interfaces which provide info about a Processor
    crow::connections::systemBus->async_method_call(
        [resp, processorId, handler = std::forward<Handler>(handler)](
            boost::system::error_code ec,
            const dbus::utility::MapperGetSubTreeResponse& subtree) mutable {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error: " << ec;
            messages::internalError(resp->res);
            return;
        }
        for (const auto& [objectPath, serviceMap] : subtree)
        {
            // Ignore any objects which don't end with our desired cpu name
            if (!objectPath.ends_with(processorId))
            {
                continue;
            }

            bool found = false;
            // Filter out objects that don't have the CPU-specific
            // interfaces to make sure we can return 404 on non-CPUs
            // (e.g. /redfish/../Processors/dimm0)
            for (const auto& [serviceName, interfaceList] : serviceMap)
            {
                if (std::find_first_of(
                        interfaceList.begin(), interfaceList.end(),
                        processorInterfaces.begin(),
                        processorInterfaces.end()) != interfaceList.end())
                {
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                continue;
            }

            // Process the first object which does match our cpu name
            // andlog_utils::getChassisLogEntr required interfaces, and
            // potentially ignore any other matching objects. Assume all
            // interfaces we want to process must be on the same object path.

            handler(objectPath, serviceMap);
            log_utils::getChassisLogEntry(resp, "/Status"_json_pointer,
                                          objectPath, "OpenBMC.0.2.0.CPUError");
            return;
        }
        messages::resourceNotFound(resp->res, "Processor", processorId);
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory", 0,
        std::array<const char*, 8>{
            "xyz.openbmc_project.Common.UUID",
            "xyz.openbmc_project.Inventory.Decorator.Asset",
            "xyz.openbmc_project.Inventory.Decorator.Revision",
            "xyz.openbmc_project.Inventory.Item.Cpu",
            "xyz.openbmc_project.Inventory.Decorator.LocationCode",
            "xyz.openbmc_project.Inventory.Item.Accelerator",
            "xyz.openbmc_project.Control.Processor.CurrentOperatingConfig",
            "xyz.openbmc_project.Inventory.Decorator.UniqueIdentifier"});
}

inline void getProcessorData(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                             const nlohmann::json::json_pointer& jsonPtr,
                             uint8_t expandLevel,
                             const std::string& processorId,
                             const std::string& objectPath,
                             const dbus::utility::MapperServiceMap& serviceMap)
{
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/Processor/Processor.json>; rel=describedby");
    aResp->res.jsonValue[jsonPtr]["@odata.type"] =
        "#Processor.v1_11_0.Processor";
    aResp->res.jsonValue[jsonPtr]["@odata.id"] = crow::utility::urlFromPieces(
        "redfish", "v1", "Systems", "system", "Processors", processorId);

    if (expandLevel > 0)
    {
        nlohmann::json::json_pointer subProcessorPtr =
            jsonPtr / "SubProcessors";
        getSubProcessorCoreCollectionWithExpand(
            aResp, subProcessorPtr, expandLevel - 1, processorId, objectPath);
    }
    else
    {
        aResp->res.jsonValue[jsonPtr]["SubProcessors"]["@odata.id"] =
            crow::utility::urlFromPieces("redfish", "v1", "Systems", "system",
                                         "Processors", processorId,
                                         "SubProcessors");
    }

    for (const auto& [serviceName, interfaceList] : serviceMap)
    {
        for (const auto& interface : interfaceList)
        {
            if (interface == "xyz.openbmc_project.Inventory.Decorator.Asset")
            {
                getCpuAssetData(aResp, serviceName, objectPath, jsonPtr);
            }
            else if (interface ==
                     "xyz.openbmc_project.Inventory.Decorator.Revision")
            {
                getCpuRevisionData(aResp, serviceName, objectPath, jsonPtr);
            }
            else if (interface == "xyz.openbmc_project.Inventory.Item.Cpu")
            {
                getCpuDataByService(aResp, processorId, serviceName, objectPath,
                                    jsonPtr);
            }
            else if (interface ==
                     "xyz.openbmc_project.Inventory.Item.Accelerator")
            {
                getAcceleratorDataByService(aResp, processorId, serviceName,
                                            objectPath, jsonPtr);
            }
            else if (
                interface ==
                "xyz.openbmc_project.Control.Processor.CurrentOperatingConfig")
            {
                getCpuConfigData(aResp, processorId, serviceName, objectPath,
                                 jsonPtr);
            }
            else if (interface ==
                     "xyz.openbmc_project.Inventory.Decorator.LocationCode")
            {
                location_util::getLocationCode(aResp, serviceName, objectPath,
                                               jsonPtr / "Location");
            }
            else if (interface == "xyz.openbmc_project.Common.UUID")
            {
                getProcessorUUID(aResp, serviceName, objectPath, jsonPtr);
            }
            else if (interface ==
                     "xyz.openbmc_project.Inventory.Decorator.UniqueIdentifier")
            {
                getCpuUniqueId(aResp, serviceName, objectPath, jsonPtr);
            }
            else
            {
                std::optional<std::string> locationType =
                    location_util::getLocationType(interface);
                if (locationType == std::nullopt)
                {
                    BMCWEB_LOG_DEBUG
                        << "getLocationType for Processor failed for "
                        << interface;
                    continue;
                }

                aResp->res.jsonValue[jsonPtr]["Location"]["PartLocation"]
                                    ["LocationType"] = *locationType;
            }
        }
    }
    getCpuChassisAssociation(aResp, processorId, objectPath, jsonPtr);
}

template <typename Handler>
inline void getProcessorPaths(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                              const std::string& processorId, Handler&& handler)
{
    crow::connections::systemBus->async_method_call(
        [processorId, aResp, handler{std::forward<Handler>(handler)}](
            const boost::system::error_code ec,
            const std::vector<std::string>& subTreePaths) {
        if (ec)
        {
            handler(ec, "");
            return;
        }

        for (const std::string& cpuPath : subTreePaths)
        {
            if (sdbusplus::message::object_path(cpuPath).filename() !=
                processorId)
            {
                continue;
            }

            handler(ec, cpuPath);
            return;
        }

        // Set an error code since valid cpu path is not found
        handler(boost::system::errc::make_error_code(
                    boost::system::errc::no_such_file_or_directory),
                "");
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
        "/xyz/openbmc_project/inventory", 0,
        std::array<const char*, 1>{"xyz.openbmc_project.Inventory.Item.Cpu"});
}

template <typename Handler>
inline void
    getSubProcessorCorePaths(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                             const std::string& processorId,
                             const std::string& coreId, Handler&& handler)
{

    crow::connections::systemBus->async_method_call(
        [processorId, coreId, aResp, handler{std::forward<Handler>(handler)}](
            const boost::system::error_code ec,
            const std::vector<std::string>& subTreeCpuPaths) {
        if (ec)
        {
            handler(ec, "");
            return;
        }

        for (const std::string& cpuPath : subTreeCpuPaths)
        {
            if (sdbusplus::message::object_path(cpuPath).filename() !=
                processorId)
            {
                continue;
            }

            crow::connections::systemBus->async_method_call(
                [processorId, coreId, aResp,
                 handler](const boost::system::error_code ec2,
                          const std::vector<std::string>& subTreeCorePaths) {
                if (ec2)
                {
                    handler(ec2, "");
                    return;
                }

                for (const std::string& corePath : subTreeCorePaths)
                {
                    if (sdbusplus::message::object_path(corePath).filename() !=
                        coreId)
                    {
                        continue;
                    }
                    handler(ec2, corePath);
                    return;
                }
                // Set an error code since valid processor core path is not
                // found
                handler(boost::system::errc::make_error_code(
                            boost::system::errc::no_such_file_or_directory),
                        "");
                },
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
                "/xyz/openbmc_project/inventory", 0,
                std::array<const char*, 1>{
                    "xyz.openbmc_project.Inventory.Item.CpuCore"});
            return;
        }

        // Set an error code since valid processor cpu path is not found
        handler(boost::system::errc::make_error_code(
                    boost::system::errc::no_such_file_or_directory),
                "");
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
        "/xyz/openbmc_project/inventory", 0,
        std::array<const char*, 1>{"xyz.openbmc_project.Inventory.Item.Cpu"});
}

inline void getCoreThreadDataByService(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const nlohmann::json::json_pointer& jsonPtr, const std::string& processorId,
    const std::string& coreId, const std::string& threadId,
    const dbus::utility::DBusInteracesMap& interfaceMap)
{
    aResp->res.jsonValue[jsonPtr]["@odata.type"] =
        "#Processor.v1_11_0.Processor";
    aResp->res.jsonValue[jsonPtr]["@odata.id"] = crow::utility::urlFromPieces(
        "redfish", "v1", "Systems", "system", "Processors", processorId,
        "SubProcessors", coreId, "SubProcessors", threadId);
    aResp->res.jsonValue[jsonPtr]["Name"] = "SubProcessor";
    aResp->res.jsonValue[jsonPtr]["Id"] = threadId;

    aResp->res.jsonValue[jsonPtr]["Status"]["State"] = "Enabled";
    aResp->res.jsonValue[jsonPtr]["Status"]["Health"] = "OK";

    bool present = false;
    bool functional = false;

    for (const auto& [interface, properties] : interfaceMap)
    {
        if (interface == "xyz.openbmc_project.State."
                         "Decorator.OperationalStatus")
        {
            for (const auto& [proName, proValue] : properties)
            {
                if (proName == "Functional")
                {
                    const bool* value = std::get_if<bool>(&proValue);
                    if (value == nullptr)
                    {
                        messages::internalError(aResp->res);
                        return;
                    }
                    functional = *value;
                }
            }
        }
        else if (interface == "xyz.openbmc_project.Inventory.Item")
        {
            for (const auto& [proName, proValue] : properties)
            {
                if (proName == "Present")
                {
                    const bool* value = std::get_if<bool>(&proValue);
                    if (value == nullptr)
                    {
                        messages::internalError(aResp->res);
                        return;
                    }
                    present = *value;
                }
                else if (proName == "PrettyName")
                {
                    const std::string* prettyName =
                        std::get_if<std::string>(&proValue);
                    if (prettyName == nullptr)
                    {
                        messages::internalError(aResp->res);
                        return;
                    }
                    aResp->res.jsonValue[jsonPtr]["Name"] = *prettyName;
                }
            }
        }
        else if (interface == "xyz.openbmc_project.Inventory.Item.CpuThread")
        {
            for (const auto& [proName, proValue] : properties)
            {
                if (proName == "Microcode")
                {
                    const uint32_t* value = std::get_if<uint32_t>(&proValue);
                    if (value == nullptr)
                    {
                        messages::internalError(aResp->res);
                        return;
                    }
                    aResp->res
                        .jsonValue[jsonPtr]["ProcessorId"]["MicrocodeInfo"] =
                        "0x" + intToHexString(*value, 8);
                }
            }
        }
    }

    if (!present)
    {
        aResp->res.jsonValue[jsonPtr]["Status"]["State"] = "Absent";
    }

    if (!functional)
    {
        aResp->res.jsonValue[jsonPtr]["Status"]["Health"] = "Critical";
    }
}

inline void getSubProcessorThreadData(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const std::string& processorId, const std::string& coreId,
    const std::string& threadId, const boost::system::error_code ec,
    const std::string& corePath)
{
    if (ec)
    {
        BMCWEB_LOG_DEBUG << "DBUS response error, ec: " << ec.value();
        // No sub processor core objects found by mapper
        if (ec.value() == boost::system::errc::io_error)
        {
            messages::resourceNotFound(aResp->res,
                                       "#Processor.v1_11_0.Processor", coreId);
            return;
        }

        messages::internalError(aResp->res);
        return;
    }
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/Processor/Processor.json>; rel=describedby");

    const std::string objPath = corePath + "/containing";
    sdbusplus::asio::getProperty<std::vector<std::string>>(
        *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper",
        objPath, "xyz.openbmc_project.Association", "endpoints",
        [aResp, coreId, threadId, processorId](
            const boost::system::error_code ec2,
            const dbus::utility::MapperGetSubTreePathsResponse& objectPaths) {
        if (ec2)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error, ec2: " << ec2.value();
            // No endpoints property found by mapper
            if (ec2.value() == boost::system::errc::io_error)
            {
                messages::resourceNotFound(
                    aResp->res, "#Processor.v1_11_0.Processor", coreId);
                return;
            }

            messages::internalError(aResp->res);
            return;
        }

        crow::connections::systemBus->async_method_call(
            [aResp, objectPaths{std::move(objectPaths)}, threadId, coreId,
             processorId](const boost::system::error_code ec3,
                          const dbus::utility::MapperGetSubTreePathsResponse&
                              subTreePaths) {
            if (ec3)
            {
                BMCWEB_LOG_DEBUG << "DBUS response error, ec3: " << ec3.value();
                if (ec3.value() == boost::system::errc::io_error)
                {
                    messages::resourceNotFound(aResp->res,
                                               "#Processor.v1_11_0.Processor",
                                               processorId);
                    return;
                }

                messages::internalError(aResp->res);
                return;
            }

            std::vector<std::string> threadPaths;

            // For a given association endpoint path, there could be associated
            // members with different interface types. So filter out the
            // required members.
            std::set_intersection(objectPaths.begin(), objectPaths.end(),
                                  subTreePaths.begin(), subTreePaths.end(),
                                  std::back_inserter(threadPaths));

            for (const std::string& threadPath : threadPaths)
            {

                if (sdbusplus::message::object_path(threadPath).filename() !=
                    threadId)
                {
                    continue;
                }

                crow::connections::systemBus->async_method_call(
                    [aResp, threadPath, coreId, threadId, processorId](
                        const boost::system::error_code ec4,
                        const dbus::utility::MapperServiceMap& serviceMap) {
                    if (ec4)
                    {
                        BMCWEB_LOG_DEBUG << "DBUS response error, ec4: "
                                         << ec4.value();
                        if (ec4.value() == boost::system::errc::io_error)
                        {
                            messages::resourceNotFound(
                                aResp->res, "#Processor.v1_11_0.Processor",
                                coreId);
                            return;
                        }

                        messages::internalError(aResp->res);
                        return;
                    }

                    if (serviceMap.empty())
                    {
                        BMCWEB_LOG_WARNING
                            << "Error in finding the service name";
                        messages::internalError(aResp->res);
                        return;
                    }

                    crow::connections::systemBus->async_method_call(
                        [threadPath, processorId, coreId, threadId, aResp](
                            const boost::system::error_code ec5,
                            const dbus::utility::ManagedObjectType& dbusData) {
                        if (ec5)
                        {
                            messages::internalError(aResp->res);
                            return;
                        }
                        for (const auto& [path, interfaces] : dbusData)
                        {
                            if (path != threadPath)
                            {
                                continue;
                            }

                            getCoreThreadDataByService(aResp, ""_json_pointer,
                                                       processorId, coreId,
                                                       threadId, interfaces);
                            return;
                        }
                        // Object not found
                        messages::resourceNotFound(
                            aResp->res, "#Processor.v1_11_0.Processor", coreId);
                        },
                        serviceMap.begin()->first,
                        "/xyz/openbmc_project/inventory",
                        "org.freedesktop.DBus.ObjectManager",
                        "GetManagedObjects");
                    },
                    "xyz.openbmc_project.ObjectMapper",
                    "/xyz/openbmc_project/object_mapper",
                    "xyz.openbmc_project.ObjectMapper", "GetObject", threadPath,
                    std::array<std::string, 0>());
                return;
            }
            // Object not found
            messages::resourceNotFound(
                aResp->res, "#Processor.v1_11_0.Processor", threadId);
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
            "/xyz/openbmc_project/inventory", 0,
            std::array<const char*, 1>{
                "xyz.openbmc_project.Inventory.Item.CpuThread"});
        });
}

inline void getSubProcessorThreadMembers(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const std::string& processorId, const std::string& coreId,
    const boost::system::error_code ec, const std::string& corePath)
{
    if (ec)
    {
        BMCWEB_LOG_DEBUG << "DBUS response error, ec: " << ec.value();
        // No sub processor core objects found by mapper
        if (ec.value() == boost::system::errc::io_error)
        {
            messages::resourceNotFound(aResp->res,
                                       "#Processor.v1_11_0.Processor", coreId);
            return;
        }

        messages::internalError(aResp->res);
        return;
    }

    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/ProcessorCollection/ProcessorCollection.json>; rel=describedby");
    aResp->res.jsonValue["@odata.type"] =
        "#ProcessorCollection.ProcessorCollection";
    aResp->res.jsonValue["@odata.id"] = crow::utility::urlFromPieces(
        "redfish", "v1", "Systems", "system", "Processors", processorId,
        "SubProcessors", coreId, "SubProcessors");
    aResp->res.jsonValue["Name"] = "SubProcessor Collection";

    const boost::urls::url& subProcessorsPath = crow::utility::urlFromPieces(
        "redfish", "v1", "Systems", "system", "Processors", processorId,
        "SubProcessors", coreId, "SubProcessors");

    const std::string associationPath = corePath + "/containing";

    collection_util::getAssociatedCollectionMembers(
        aResp, subProcessorsPath,
        std::vector<const char*>{
            "xyz.openbmc_project.Inventory.Item.CpuThread"},
        associationPath.c_str());
}

inline void
    getCpuCoreDataByService(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                            const nlohmann::json::json_pointer& jsonPtr,
                            uint8_t expandLevel, const std::string& processorId,
                            const std::string& coreId,
                            const dbus::utility::DBusInteracesMap& interfaceMap,
                            const std::string& corePath)
{
    aResp->res.jsonValue[jsonPtr]["@odata.type"] =
        "#Processor.v1_11_0.Processor";
    aResp->res.jsonValue[jsonPtr]["@odata.id"] = crow::utility::urlFromPieces(
        "redfish", "v1", "Systems", "system", "Processors", processorId,
        "SubProcessors", coreId);

    aResp->res.jsonValue[jsonPtr]["Name"] = "SubProcessor";
    aResp->res.jsonValue[jsonPtr]["Id"] = coreId;

    if (expandLevel > 0)
    {
        nlohmann::json::json_pointer subProcessorPtr =
            jsonPtr / "SubProcessors";
        getSubProcessorThreadCollectionWithExpand(aResp, subProcessorPtr,
                                                  expandLevel - 1, processorId,
                                                  coreId, corePath);
    }
    else
    {
        aResp->res.jsonValue[jsonPtr]["SubProcessors"]["@odata.id"] =
            crow::utility::urlFromPieces(
                "redfish", "v1", "Systems", "system", "Processors", processorId,
                "SubProcessors", coreId, "SubProcessors");
    }

    aResp->res.jsonValue[jsonPtr]["Status"]["State"] = "Enabled";
    aResp->res.jsonValue[jsonPtr]["Status"]["Health"] = "OK";

    bool present = false;
    bool functional = false;

    for (const auto& [interface, properties] : interfaceMap)
    {
        if (interface == "xyz.openbmc_project.State."
                         "Decorator.OperationalStatus")
        {
            for (const auto& [proName, proValue] : properties)
            {
                if (proName == "Functional")
                {
                    const bool* value = std::get_if<bool>(&proValue);
                    if (value == nullptr)
                    {
                        messages::internalError(aResp->res);
                        return;
                    }
                    functional = *value;
                }
            }
        }
        else if (interface == "xyz.openbmc_project.Inventory.Item")
        {
            for (const auto& [proName, proValue] : properties)
            {
                if (proName == "Present")
                {
                    const bool* value = std::get_if<bool>(&proValue);
                    if (value == nullptr)
                    {
                        messages::internalError(aResp->res);
                        return;
                    }
                    present = *value;
                }
                else if (proName == "PrettyName")
                {
                    const std::string* prettyName =
                        std::get_if<std::string>(&proValue);
                    if (prettyName == nullptr)
                    {
                        messages::internalError(aResp->res);
                        return;
                    }
                    aResp->res.jsonValue[jsonPtr]["Name"] = *prettyName;
                }
            }
        }
        else if (interface == "xyz.openbmc_project.Inventory.Item.CpuCore")
        {
            for (const auto& [proName, proValue] : properties)
            {
                if (proName == "Microcode")
                {
                    const uint32_t* value = std::get_if<uint32_t>(&proValue);
                    if (value == nullptr)
                    {
                        messages::internalError(aResp->res);
                        return;
                    }
                    aResp->res
                        .jsonValue[jsonPtr]["ProcessorId"]["MicrocodeInfo"] =
                        "0x" + intToHexString(*value, 8);
                }
            }
        }
    }

    if (!present)
    {
        aResp->res.jsonValue[jsonPtr]["Status"]["State"] = "Absent";
    }

    if (!functional)
    {
        aResp->res.jsonValue[jsonPtr]["Status"]["Health"] = "Critical";
    }
}

inline void getSubProcessorCoreData(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const std::string& processorId, const std::string& coreId,
    const boost::system::error_code ec, const std::string& cpuPath)
{
    if (ec)
    {
        BMCWEB_LOG_DEBUG << "DBUS response error, ec: " << ec.value();
        // No processor objects found by mapper
        if (ec.value() == boost::system::errc::io_error)
        {
            messages::resourceNotFound(
                aResp->res, "#Processor.v1_11_0.Processor", processorId);
            return;
        }

        messages::internalError(aResp->res);
        return;
    }
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/Processor/Processor.json>; rel=describedby");

    const std::string objPath = cpuPath + "/containing";
    sdbusplus::asio::getProperty<std::vector<std::string>>(
        *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper",
        objPath, "xyz.openbmc_project.Association", "endpoints",
        [aResp, processorId, coreId](
            const boost::system::error_code ec2,
            const dbus::utility::MapperGetSubTreePathsResponse& objectPaths) {
        if (ec2)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error, ec2: " << ec2.value();
            // No endpoints property found by mapper
            if (ec2.value() == boost::system::errc::io_error)
            {
                messages::resourceNotFound(
                    aResp->res, "#Processor.v1_11_0.Processor", processorId);
                return;
            }

            messages::internalError(aResp->res);
            return;
        }

        crow::connections::systemBus->async_method_call(
            [aResp, objectPaths{std::move(objectPaths)}, coreId,
             processorId](const boost::system::error_code ec3,
                          const dbus::utility::MapperGetSubTreePathsResponse&
                              subTreePaths) {
            if (ec3)
            {
                BMCWEB_LOG_DEBUG << "DBUS response error, ec3: " << ec3.value();
                if (ec3.value() == boost::system::errc::io_error)
                {
                    messages::resourceNotFound(aResp->res,
                                               "#Processor.v1_11_0.Processor",
                                               processorId);
                    return;
                }

                messages::internalError(aResp->res);
                return;
            }

            std::vector<std::string> corePaths;

            // For a given association endpoint path, there could be associated
            // members with different interface types. So filter out the
            // required members.
            std::set_intersection(objectPaths.begin(), objectPaths.end(),
                                  subTreePaths.begin(), subTreePaths.end(),
                                  std::back_inserter(corePaths));

            for (const std::string& corePath : corePaths)
            {
                if (sdbusplus::message::object_path(corePath).filename() !=
                    coreId)
                {
                    continue;
                }

                crow::connections::systemBus->async_method_call(
                    [aResp, corePath, processorId, coreId](
                        const boost::system::error_code ec4,
                        const dbus::utility::MapperServiceMap& serviceMap) {
                    if (ec4)
                    {
                        BMCWEB_LOG_DEBUG << "DBUS response error, ec4: "
                                         << ec4.value();
                        if (ec4.value() == boost::system::errc::io_error)
                        {
                            messages::resourceNotFound(
                                aResp->res, "#Processor.v1_11_0.Processor",
                                processorId);
                            return;
                        }

                        messages::internalError(aResp->res);
                        return;
                    }

                    if (serviceMap.empty())
                    {
                        BMCWEB_LOG_WARNING
                            << "Error in finding the service name";
                        messages::internalError(aResp->res);
                        return;
                    }

                    crow::connections::systemBus->async_method_call(
                        [corePath, processorId, coreId, aResp](
                            const boost::system::error_code ec5,
                            const dbus::utility::ManagedObjectType& dbusData) {
                        if (ec5)
                        {
                            messages::internalError(aResp->res);
                            return;
                        }
                        for (const auto& [path, interfaces] : dbusData)
                        {
                            if (path != corePath)
                            {
                                continue;
                            }

                            getCpuCoreDataByService(aResp, ""_json_pointer, 0,
                                                    processorId, coreId,
                                                    interfaces, corePath);
                            return;
                        }
                        // Object not found
                        messages::resourceNotFound(
                            aResp->res, "#Processor.v1_11_0.Processor", coreId);
                        },
                        serviceMap.begin()->first,
                        "/xyz/openbmc_project/inventory",
                        "org.freedesktop.DBus.ObjectManager",
                        "GetManagedObjects");
                    },
                    "xyz.openbmc_project.ObjectMapper",
                    "/xyz/openbmc_project/object_mapper",
                    "xyz.openbmc_project.ObjectMapper", "GetObject", corePath,
                    std::array<std::string, 0>());
                return;
            }
            // Object not found
            messages::resourceNotFound(aResp->res,
                                       "#Processor.v1_11_0.Processor", coreId);
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
            "/xyz/openbmc_project/inventory", 0,
            std::array<const char*, 1>{
                "xyz.openbmc_project.Inventory.Item.CpuCore"});
        });
}

inline void
    getSubProcessorCoreMembers(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                               const std::string& processorId,
                               const boost::system::error_code ec,
                               const std::string& cpuPath)
{
    if (ec)
    {
        BMCWEB_LOG_DEBUG << "DBUS response error, ec: " << ec.value();
        // No processor objects found by mapper
        if (ec.value() == boost::system::errc::io_error)
        {
            messages::resourceNotFound(
                aResp->res, "#Processor.v1_11_0.Processor", processorId);
            return;
        }

        messages::internalError(aResp->res);
        return;
    }

    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/ProcessorCollection/ProcessorCollection.json>; rel=describedby");
    aResp->res.jsonValue["@odata.type"] =
        "#ProcessorCollection.ProcessorCollection";
    aResp->res.jsonValue["@odata.id"] = crow::utility::urlFromPieces(
        "redfish", "v1", "Systems", "system", "Processors", processorId,
        "SubProcessors");
    aResp->res.jsonValue["Name"] = "SubProcessor Collection";

    const boost::urls::url& subProcessorsPath = crow::utility::urlFromPieces(
        "redfish", "v1", "Systems", "system", "Processors", processorId,
        "SubProcessors");

    const std::string associationPath = cpuPath + "/containing";

    collection_util::getAssociatedCollectionMembers(
        aResp, subProcessorsPath,
        std::vector<const char*>{"xyz.openbmc_project.Inventory.Item.CpuCore"},
        associationPath.c_str());
}

inline void getSubProcessorThreadCollectionWithExpand(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const nlohmann::json::json_pointer& jsonPtr, uint8_t expandLevel,
    const std::string& processorId, const std::string& coreId,
    const std::string& corePath)
{
    const std::string objPath = corePath + "/containing";
    sdbusplus::asio::getProperty<std::vector<std::string>>(
        *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper",
        objPath, "xyz.openbmc_project.Association", "endpoints",
        [aResp{aResp}, processorId, coreId, expandLevel, jsonPtr](
            const boost::system::error_code ec,
            const dbus::utility::MapperGetSubTreePathsResponse& objectPaths) {
        if (ec == boost::system::errc::io_error)
        {
            aResp->res.jsonValue[jsonPtr]["Members"] = nlohmann::json::array();
            aResp->res.jsonValue[jsonPtr]["Members@odata.count"] = 0;
            return;
        }

        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error " << ec.value();
            messages::internalError(aResp->res);
            return;
        }

        const std::vector<const char*> cpuThreadInterfaces{
            "xyz.openbmc_project.Inventory.Item.CpuThread"};

        // For a given association endpoint path, there could be associated
        // members with different interface types. Collect these object paths in
        // an unordered_map to filter desired thread paths from the GetSubTree
        // response.
        std::unordered_map<std::string, std::string> objectPathsMap;

        for (const std::string& path : objectPaths)
        {
            objectPathsMap.insert({path, path});
        }

        crow::connections::systemBus->async_method_call(
            [coreId, processorId, objectPathsMap{std::move(objectPathsMap)},
             jsonPtr, expandLevel, aResp{aResp}](
                const boost::system::error_code ec2,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec2 == boost::system::errc::io_error)
            {
                aResp->res.jsonValue[jsonPtr]["Members"] =
                    nlohmann::json::array();
                aResp->res.jsonValue[jsonPtr]["Members@odata.count"] = 0;
                return;
            }

            if (ec2)
            {
                BMCWEB_LOG_DEBUG << "DBUS response error " << ec2.value();
                messages::internalError(aResp->res);
                return;
            }

            aResp->res.jsonValue[jsonPtr]["@odata.type"] =
                "#ProcessorCollection.ProcessorCollection";
            aResp->res.jsonValue[jsonPtr]["@odata.id"] =
                crow::utility::urlFromPieces(
                    "redfish", "v1", "Systems", "system", "Processors",
                    processorId, "SubProcessors", coreId, "SubProcessors");

            // Vector that stores numerically sorted thread IDs
            std::vector<std::string> threads;

            // Container that maps thread ID to [threadObjectPath,
            // serviceMap]
            resourceIdToSubtreeRespMapType threadIdToSubtreeRespMap;

            for (const auto& [threadObjectPath, serviceMap] : subtree)
            {
                // Filter out the desired threads
                if (objectPathsMap.find(threadObjectPath) ==
                    objectPathsMap.end())
                {
                    continue;
                }
                sdbusplus::message::object_path threadPath(threadObjectPath);
                const std::string& threadId = threadPath.filename();
                threadIdToSubtreeRespMap.insert(
                    {threadId, std::make_pair(threadObjectPath, serviceMap)});
                threads.push_back(threadId);
            }

            std::string subProcessorsPath =
                "/redfish/v1/Systems/system/Processors/" + processorId +
                "/SubProcessors/" + coreId + "/SubProcessors";

            // Get numerically sorted list of thread IDs
            std::sort(threads.begin(), threads.end(),
                      AlphanumLess<std::string>());
            size_t threadCount = threads.size();

            const dbus::utility::MapperServiceMap& serviceMap =
                std::get<1>(threadIdToSubtreeRespMap.begin()->second);
            if (serviceMap.empty())
            {
                BMCWEB_LOG_WARNING << "Error in finding the service name";
                messages::internalError(aResp->res);
                return;
            }

            std::string serviceName = serviceMap.begin()->first;

            crow::connections::systemBus->async_method_call(
                [processorId, coreId, threads{std::move(threads)},
                 threadIdToSubtreeRespMap{std::move(threadIdToSubtreeRespMap)},
                 jsonPtr, expandLevel, aResp](
                    const boost::system::error_code ec3,
                    const dbus::utility::ManagedObjectType& dbusData) mutable {
                if (ec3)
                {
                    BMCWEB_LOG_DEBUG << "DBUS response error, ec3: "
                                     << ec3.value();
                    messages::internalError(aResp->res);
                    return;
                }

                // Container to store mapping of threadPath to Interfaces
                std::unordered_map<std::string, dbus::utility::DBusInteracesMap>
                    threadPathToInterfacesMap;

                // Collect the threadPath to Interfaces mapping, to avoid
                // running GetManagedObjects call for each thread
                for (const auto& [threadPath, interfaces] : dbusData)
                {
                    threadPathToInterfacesMap.insert({threadPath, interfaces});
                }

                size_t threadMemberCount = 0;
                for (const std::string& threadId : threads)
                {
                    nlohmann::json::json_pointer threadMemberPtr =
                        jsonPtr / "Members" / threadMemberCount;

                    if (expandLevel > 0)
                    {
                        const std::string& threadPath =
                            threadIdToSubtreeRespMap[threadId].first;
                        const dbus::utility::MapperServiceMap& serviceMap2 =
                            threadIdToSubtreeRespMap[threadId].second;

                        if (serviceMap2.empty())
                        {
                            BMCWEB_LOG_WARNING
                                << "Error in finding the service name";
                            messages::internalError(aResp->res);
                            return;
                        }

                        getCoreThreadDataByService(
                            aResp, threadMemberPtr, processorId, coreId,
                            threadId, threadPathToInterfacesMap[threadPath]);
                    }
                    else
                    {
                        aResp->res.jsonValue[threadMemberPtr]["@odata.id"] =
                            crow::utility::urlFromPieces(
                                "redfish", "v1", "Systems", "system",
                                "Processors", processorId, "SubProcessors",
                                coreId, "SubProcessors", threadId);
                    }
                    threadMemberCount++;
                }
                },
                serviceName, "/xyz/openbmc_project/inventory",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

            aResp->res.jsonValue[jsonPtr]["Members@odata.count"] = threadCount;
            aResp->res.jsonValue[jsonPtr]["Name"] = "SubProcessor Collection";
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree",
            "/xyz/openbmc_project/inventory", 0, cpuThreadInterfaces);
        });
}

inline void getSubProcessorCoreCollectionWithExpand(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const nlohmann::json::json_pointer& jsonPtr, uint8_t expandLevel,
    const std::string& processorId, const std::string& cpuPath)
{
    const std::string objPath = cpuPath + "/containing";
    sdbusplus::asio::getProperty<std::vector<std::string>>(
        *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper",
        objPath, "xyz.openbmc_project.Association", "endpoints",
        [aResp{aResp}, processorId, expandLevel, jsonPtr](
            const boost::system::error_code ec,
            const dbus::utility::MapperGetSubTreePathsResponse& objectPaths) {
        if (ec == boost::system::errc::io_error)
        {
            aResp->res.jsonValue[jsonPtr]["Members"] = nlohmann::json::array();
            aResp->res.jsonValue[jsonPtr]["Members@odata.count"] = 0;
            return;
        }

        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error " << ec.value();
            messages::internalError(aResp->res);
            return;
        }

        const std::vector<const char*> cpuCoreInterfaces{
            "xyz.openbmc_project.Inventory.Item.CpuCore"};

        // For a given association endpoint path, there could be associated
        // members with different interface types. Collect these object paths in
        // an unordered_map to filter desired core paths from the GetSubTree
        // response.
        std::unordered_map<std::string, std::string> objectPathsMap;

        for (const std::string& path : objectPaths)
        {
            objectPathsMap.insert({path, path});
        }

        crow::connections::systemBus->async_method_call(
            [processorId, objectPathsMap{std::move(objectPathsMap)}, jsonPtr,
             expandLevel, aResp{aResp}](
                const boost::system::error_code ec2,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec2 == boost::system::errc::io_error)
            {
                aResp->res.jsonValue[jsonPtr]["Members"] =
                    nlohmann::json::array();
                aResp->res.jsonValue[jsonPtr]["Members@odata.count"] = 0;
                return;
            }

            if (ec2)
            {
                BMCWEB_LOG_DEBUG << "DBUS response error " << ec2.value();
                messages::internalError(aResp->res);
                return;
            }

            aResp->res.jsonValue[jsonPtr]["@odata.type"] =
                "#ProcessorCollection.ProcessorCollection";
            aResp->res.jsonValue[jsonPtr]["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Systems",
                                             "system", "Processors",
                                             processorId, "SubProcessors");

            // Vector that stores  numerically sorted core IDs
            std::vector<std::string> cores;

            // Container that maps core ID to [coreObjectPath,
            // serviceMap]
            resourceIdToSubtreeRespMapType coreIdToSubtreeRespMap;
            for (const auto& [coreObjectPath, serviceMap] : subtree)
            {
                // Filter out the desired cores
                if (objectPathsMap.find(coreObjectPath) == objectPathsMap.end())
                {
                    continue;
                }
                sdbusplus::message::object_path corePath(coreObjectPath);
                const std::string& coreId = corePath.filename();
                coreIdToSubtreeRespMap.insert(
                    {coreId, std::make_pair(coreObjectPath, serviceMap)});
                cores.push_back(coreId);
            }

            std::string subProcessorsPath =
                "/redfish/v1/Systems/system/Processors/" + processorId +
                "/SubProcessors";

            // Get numerically sorted list of core IDs
            std::sort(cores.begin(), cores.end(), AlphanumLess<std::string>());
            size_t coreCount = cores.size();

            const dbus::utility::MapperServiceMap& serviceMap =
                std::get<1>(coreIdToSubtreeRespMap.begin()->second);
            if (serviceMap.empty())
            {
                BMCWEB_LOG_WARNING << "Error in finding the service name";
                messages::internalError(aResp->res);
                return;
            }

            std::string serviceName = serviceMap.begin()->first;

            crow::connections::systemBus->async_method_call(
                [processorId, cores{std::move(cores)},
                 coreIdToSubtreeRespMap{std::move(coreIdToSubtreeRespMap)},
                 jsonPtr, expandLevel, aResp](
                    const boost::system::error_code ec3,
                    const dbus::utility::ManagedObjectType& dbusData) mutable {
                if (ec3)
                {
                    BMCWEB_LOG_DEBUG << "DBUS response error, ec3: "
                                     << ec3.value();
                    messages::internalError(aResp->res);
                    return;
                }

                // Container to store mapping of corePath to Interfaces
                std::unordered_map<std::string, dbus::utility::DBusInteracesMap>
                    corePathToInterfacesMap;

                // Collect the corePath to Interfaces mapping, to avoid
                // running GetManagedObjects call for each core
                for (const auto& [corePath, interfaces] : dbusData)
                {
                    corePathToInterfacesMap.insert({corePath, interfaces});
                }

                size_t coreMemberCount = 0;
                for (const std::string& coreId : cores)
                {
                    nlohmann::json::json_pointer coreMemberPtr =
                        jsonPtr / "Members" / coreMemberCount;
                    if (expandLevel > 0)
                    {

                        const std::string& corePath =
                            (coreIdToSubtreeRespMap[coreId]).first;
                        const dbus::utility::MapperServiceMap& serviceMap2 =
                            (coreIdToSubtreeRespMap[coreId]).second;

                        if (serviceMap2.empty())
                        {
                            BMCWEB_LOG_WARNING
                                << "Error in finding the service name";
                            messages::internalError(aResp->res);
                            return;
                        }

                        getCpuCoreDataByService(
                            aResp, coreMemberPtr, expandLevel - 1, processorId,
                            coreId, corePathToInterfacesMap[corePath],
                            corePath);
                    }
                    else
                    {
                        aResp->res.jsonValue[coreMemberPtr]["@odata.id"] =
                            crow::utility::urlFromPieces(
                                "redfish", "v1", "Systems", "system",
                                "Processors", processorId, "SubProcessors",
                                coreId);
                    }
                    coreMemberCount++;
                }
                },
                serviceName, "/xyz/openbmc_project/inventory",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

            aResp->res.jsonValue[jsonPtr]["Members@odata.count"] = coreCount;
            aResp->res.jsonValue[jsonPtr]["Name"] = "SubProcessor Collection";
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree",
            "/xyz/openbmc_project/inventory", 0, cpuCoreInterfaces);
        });
}

inline void getProcessorCollectionWithExpand(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp, uint8_t expandLevel)
{

    crow::connections::systemBus->async_method_call(
        [expandLevel,
         aResp{aResp}](const boost::system::error_code ec,
                       const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec == boost::system::errc::io_error)
        {
            aResp->res.jsonValue["Members"] = nlohmann::json::array();
            aResp->res.jsonValue["Members@odata.count"] = 0;
            return;
        }

        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error " << ec.value();
            messages::internalError(aResp->res);
            return;
        }
        aResp->res.jsonValue["@odata.id"] =
            "/redfish/v1/Systems/system/Processors";
        aResp->res.jsonValue["@odata.type"] =
            "#ProcessorCollection.ProcessorCollection";

        // Vector to store numerically sorted cpu IDs
        std::vector<std::string> cpus;

        // Container to map cpu ID to [cpuObjectPath, serviceMap]
        std::unordered_map<
            std::string,
            std::pair<std::string, dbus::utility::MapperServiceMap>>
            cpuNameToSubtreeRespMap;
        for (const auto& [objectPath, serviceMap] : subtree)
        {
            sdbusplus::message::object_path cpuPath(objectPath);
            const std::string& cpuId = cpuPath.filename();
            cpuNameToSubtreeRespMap.insert(
                {cpuId, std::make_pair(objectPath, serviceMap)});
            cpus.push_back(cpuId);
        }

        // Get numerically sorted list of cpu IDs
        std::sort(cpus.begin(), cpus.end(), AlphanumLess<std::string>());

        size_t cpuMemberCount = 0;
        for (std::string& cpu : cpus)
        {
            nlohmann::json::json_pointer cpuMemberPtr =
                "/Members"_json_pointer / cpuMemberCount;
            std::string& objectPath = (cpuNameToSubtreeRespMap[cpu]).first;
            dbus::utility::MapperServiceMap& serviceMap =
                (cpuNameToSubtreeRespMap[cpu]).second;
            getProcessorData(aResp, cpuMemberPtr, expandLevel - 1, cpu,
                             objectPath, serviceMap);

            cpuMemberCount++;
        }

        aResp->res.jsonValue["Members@odata.count"] = cpuMemberCount;
        aResp->res.jsonValue["Name"] = "Processor Collection";
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory", 0,
        std::array<const char*, 3>{
            "xyz.openbmc_project.Inventory.Item.Cpu",
            "xyz.openbmc_project.Inventory.Item.Accelerator",
            "xyz.openbmc_project.Inventory.Decorator.UniqueIdentifier"});
}

/**
 * Request all the properties for the given D-Bus object and fill out the
 * related entries in the Redfish OperatingConfig response.
 *
 * @param[in,out]   aResp       Async HTTP response.
 * @param[in]       service     D-Bus service name to query.
 * @param[in]       objPath     D-Bus object to query.
 */
inline void
    getOperatingConfigData(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                           const std::string& service,
                           const std::string& objPath)
{
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, service, objPath,
        "xyz.openbmc_project.Inventory.Item.Cpu.OperatingConfig",
        [aResp](const boost::system::error_code ec,
                const dbus::utility::DBusPropertiesMap& properties) {
        if (ec)
        {
            BMCWEB_LOG_WARNING << "D-Bus error: " << ec << ", " << ec.message();
            messages::internalError(aResp->res);
            return;
        }

        const size_t* availableCoreCount = nullptr;
        const uint32_t* baseSpeed = nullptr;
        const uint32_t* maxJunctionTemperature = nullptr;
        const uint32_t* maxSpeed = nullptr;
        const uint32_t* powerLimit = nullptr;
        const TurboProfileProperty* turboProfile = nullptr;
        const BaseSpeedPrioritySettingsProperty* baseSpeedPrioritySettings =
            nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), properties, "AvailableCoreCount",
            availableCoreCount, "BaseSpeed", baseSpeed,
            "MaxJunctionTemperature", maxJunctionTemperature, "MaxSpeed",
            maxSpeed, "PowerLimit", powerLimit, "TurboProfile", turboProfile,
            "BaseSpeedPrioritySettings", baseSpeedPrioritySettings);

        if (!success)
        {
            messages::internalError(aResp->res);
            return;
        }

        nlohmann::json& json = aResp->res.jsonValue;

        if (availableCoreCount != nullptr)
        {
            json["TotalAvailableCoreCount"] = *availableCoreCount;
        }

        if (baseSpeed != nullptr)
        {
            json["BaseSpeedMHz"] = *baseSpeed;
        }

        if (maxJunctionTemperature != nullptr)
        {
            json["MaxJunctionTemperatureCelsius"] = *maxJunctionTemperature;
        }

        if (maxSpeed != nullptr)
        {
            json["MaxSpeedMHz"] = *maxSpeed;
        }

        if (powerLimit != nullptr)
        {
            json["TDPWatts"] = *powerLimit;
        }

        if (turboProfile != nullptr)
        {
            nlohmann::json& turboArray = json["TurboProfile"];
            turboArray = nlohmann::json::array();
            for (const auto& [turboSpeed, coreCount] : *turboProfile)
            {
                nlohmann::json::object_t turbo;
                turbo["ActiveCoreCount"] = coreCount;
                turbo["MaxSpeedMHz"] = turboSpeed;
                turboArray.push_back(std::move(turbo));
            }
        }

        if (baseSpeedPrioritySettings != nullptr)
        {
            nlohmann::json& baseSpeedArray = json["BaseSpeedPrioritySettings"];
            baseSpeedArray = nlohmann::json::array();
            for (const auto& [baseSpeedMhz, coreList] :
                 *baseSpeedPrioritySettings)
            {
                nlohmann::json::object_t speed;
                speed["CoreCount"] = coreList.size();
                speed["CoreIDs"] = coreList;
                speed["BaseSpeedMHz"] = baseSpeedMhz;
                baseSpeedArray.push_back(std::move(speed));
            }
        }
        });
}

/**
 * Handle the D-Bus response from attempting to set the CPU's AppliedConfig
 * property. Main task is to translate error messages into Redfish errors.
 *
 * @param[in,out]   resp    HTTP response.
 * @param[in]       setPropVal  Value which we attempted to set.
 * @param[in]       ec      D-Bus response error code.
 * @param[in]       msg     D-Bus response message.
 */
inline void
    handleAppliedConfigResponse(const std::shared_ptr<bmcweb::AsyncResp>& resp,
                                const std::string& setPropVal,
                                boost::system::error_code ec,
                                const sdbusplus::message_t& msg)
{
    if (!ec)
    {
        BMCWEB_LOG_DEBUG << "Set Property succeeded";
        return;
    }

    BMCWEB_LOG_DEBUG << "Set Property failed: " << ec;

    const sd_bus_error* dbusError = msg.get_error();
    if (dbusError == nullptr)
    {
        messages::internalError(resp->res);
        return;
    }

    // The asio error code doesn't know about our custom errors, so we have to
    // parse the error string. Some of these D-Bus -> Redfish translations are a
    // stretch, but it's good to try to communicate something vaguely useful.
    if (strcmp(dbusError->name,
               "xyz.openbmc_project.Common.Error.InvalidArgument") == 0)
    {
        // Service did not like the object_path we tried to set.
        messages::propertyValueIncorrect(
            resp->res, "AppliedOperatingConfig/@odata.id", setPropVal);
    }
    else if (strcmp(dbusError->name,
                    "xyz.openbmc_project.Common.Error.NotAllowed") == 0)
    {
        // Service indicates we can never change the config for this processor.
        messages::propertyNotWritable(resp->res, "AppliedOperatingConfig");
    }
    else if (strcmp(dbusError->name,
                    "xyz.openbmc_project.Common.Error.Unavailable") == 0)
    {
        // Service indicates the config cannot be changed right now, but maybe
        // in a different system state.
        messages::resourceInStandby(resp->res);
    }
    else
    {
        messages::internalError(resp->res);
    }
}

/**
 * Handle the PATCH operation of the AppliedOperatingConfig property. Do basic
 * validation of the input data, and then set the D-Bus property.
 *
 * @param[in,out]   resp            Async HTTP response.
 * @param[in]       processorId     Processor's Id.
 * @param[in]       appliedConfigUri    New property value to apply.
 * @param[in]       cpuObjectPath   Path of CPU object to modify.
 * @param[in]       serviceMap      Service map for CPU object.
 */
inline void patchAppliedOperatingConfig(
    const std::shared_ptr<bmcweb::AsyncResp>& resp,
    const std::string& processorId, const std::string& appliedConfigUri,
    const std::string& cpuObjectPath,
    const dbus::utility::MapperServiceMap& serviceMap)
{
    // Check that the property even exists by checking for the interface
    const std::string* controlService = nullptr;
    for (const auto& [serviceName, interfaceList] : serviceMap)
    {
        if (std::find(interfaceList.begin(), interfaceList.end(),
                      "xyz.openbmc_project.Control.Processor."
                      "CurrentOperatingConfig") != interfaceList.end())
        {
            controlService = &serviceName;
            break;
        }
    }

    if (controlService == nullptr)
    {
        messages::internalError(resp->res);
        return;
    }

    // Check that the config URI is a child of the cpu URI being patched.
    std::string expectedPrefix("/redfish/v1/Systems/system/Processors/");
    expectedPrefix += processorId;
    expectedPrefix += "/OperatingConfigs/";
    if (!appliedConfigUri.starts_with(expectedPrefix) ||
        expectedPrefix.size() == appliedConfigUri.size())
    {
        messages::propertyValueIncorrect(
            resp->res, "AppliedOperatingConfig/@odata.id", appliedConfigUri);
        return;
    }

    // Generate the D-Bus path of the OperatingConfig object, by assuming it's a
    // direct child of the CPU object.
    // Strip the expectedPrefix from the config URI to get the "filename", and
    // append to the CPU's path.
    std::string configBaseName = appliedConfigUri.substr(expectedPrefix.size());
    sdbusplus::message::object_path configPath(cpuObjectPath);
    configPath /= configBaseName;

    BMCWEB_LOG_INFO << "Setting config to " << configPath.str;

    // Set the property, with handler to check error responses
    crow::connections::systemBus->async_method_call(
        [resp, appliedConfigUri](const boost::system::error_code ec,
                                 const sdbusplus::message_t& msg) {
        handleAppliedConfigResponse(resp, appliedConfigUri, ec, msg);
        },
        *controlService, cpuObjectPath, "org.freedesktop.DBus.Properties",
        "Set", "xyz.openbmc_project.Control.Processor.CurrentOperatingConfig",
        "AppliedConfig", dbus::utility::DbusVariantType(std::move(configPath)));
}

inline void handleSubProcessorThreadHead(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const std::string& /* systemName */, const std::string& /* processorId */,
    const std::string& /* coreId */, const std::string& /* threadId */)
{
    if (!redfish::setUpRedfishRoute(app, req, aResp))
    {
        return;
    }
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/Processor/Processor.json>; rel=describedby");
}

inline void handleSubProcessorThreadCollectionHead(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const std::string& /* systemName */, const std::string& /* processorId */,
    const std::string& /* coreId */)
{
    if (!redfish::setUpRedfishRoute(app, req, aResp))
    {
        return;
    }
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/ProcessorCollection/ProcessorCollection.json>; rel=describedby");
}

inline void
    handleSubProcessorCoreHead(crow::App& app, const crow::Request& req,
                               const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                               const std::string& /* systemName */,
                               const std::string& /* processorId */,
                               const std::string& /* coreId */)
{
    if (!redfish::setUpRedfishRoute(app, req, aResp))
    {
        return;
    }
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/Processor/Processor.json>; rel=describedby");
}

inline void handleSubProcessorCoreCollectionHead(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const std::string& /* systemName */, const std::string& /* processorId */)
{
    if (!redfish::setUpRedfishRoute(app, req, aResp))
    {
        return;
    }
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/ProcessorCollection/ProcessorCollection.json>; rel=describedby");
}

inline void handleProcessorHead(crow::App& app, const crow::Request& req,
                                const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                                const std::string& /* systemName */,
                                const std::string& /* processorId */)
{
    if (!redfish::setUpRedfishRoute(app, req, aResp))
    {
        return;
    }
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/Processor/Processor.json>; rel=describedby");
}

inline void handleProcessorCollectionHead(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const std::string& /* systemName */)
{
    if (!redfish::setUpRedfishRoute(app, req, aResp))
    {
        return;
    }
    aResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/ProcessorCollection/ProcessorCollection.json>; rel=describedby");
}

inline void requestRoutesOperatingConfigCollection(App& app)
{

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/Processors/<str>/OperatingConfigs/")
        .privileges(redfish::privileges::getOperatingConfigCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& cpuName) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        asyncResp->res.jsonValue["@odata.type"] =
            "#OperatingConfigCollection.OperatingConfigCollection";
        asyncResp->res.jsonValue["@odata.id"] = req.url;
        asyncResp->res.jsonValue["Name"] = "Operating Config Collection";

        // First find the matching CPU object so we know how to
        // constrain our search for related Config objects.
        crow::connections::systemBus->async_method_call(
            [asyncResp, cpuName](
                const boost::system::error_code ec,
                const dbus::utility::MapperGetSubTreePathsResponse& objects) {
            if (ec)
            {
                BMCWEB_LOG_WARNING << "D-Bus error: " << ec << ", "
                                   << ec.message();
                messages::internalError(asyncResp->res);
                return;
            }

            for (const std::string& object : objects)
            {
                if (!object.ends_with(cpuName))
                {
                    continue;
                }

                // Not expected that there will be multiple matching
                // CPU objects, but if there are just use the first
                // one.

                // Use the common search routine to construct the
                // Collection of all Config objects under this CPU.
                collection_util::getCollectionMembers(
                    asyncResp,
                    crow::utility::urlFromPieces("redfish", "v1", "Systems",
                                                 "system", "Processors",
                                                 cpuName, "OperatingConfigs"),
                    {"xyz.openbmc_project.Inventory.Item.Cpu.OperatingConfig"},
                    object.c_str());
                return;
            }
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
            "/xyz/openbmc_project/inventory", 0,
            std::array<const char*, 1>{
                "xyz.openbmc_project.Control.Processor.CurrentOperatingConfig"});
        });
}

inline void requestRoutesOperatingConfig(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/system/Processors/<str>/OperatingConfigs/<str>/")
        .privileges(redfish::privileges::getOperatingConfig)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& cpuName, const std::string& configName) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        // Ask for all objects implementing OperatingConfig so we can search
        // for one with a matching name
        crow::connections::systemBus->async_method_call(
            [asyncResp, cpuName, configName, reqUrl{req.url}](
                boost::system::error_code ec,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec)
            {
                BMCWEB_LOG_WARNING << "D-Bus error: " << ec << ", "
                                   << ec.message();
                messages::internalError(asyncResp->res);
                return;
            }
            const std::string expectedEnding = cpuName + '/' + configName;
            for (const auto& [objectPath, serviceMap] : subtree)
            {
                // Ignore any configs without matching cpuX/configY
                if (!objectPath.ends_with(expectedEnding) || serviceMap.empty())
                {
                    continue;
                }

                nlohmann::json& json = asyncResp->res.jsonValue;
                json["@odata.type"] = "#OperatingConfig.v1_0_0.OperatingConfig";
                json["@odata.id"] = reqUrl;
                json["Name"] = "Processor Profile";
                json["Id"] = configName;

                // Just use the first implementation of the object - not
                // expected that there would be multiple matching
                // services
                getOperatingConfigData(asyncResp, serviceMap.begin()->first,
                                       objectPath);
                return;
            }
            messages::resourceNotFound(asyncResp->res, "OperatingConfig",
                                       configName);
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree",
            "/xyz/openbmc_project/inventory", 0,
            std::array<const char*, 1>{
                "xyz.openbmc_project.Inventory.Item.Cpu.OperatingConfig"});
        });
}

inline void requestRoutesProcessorCollection(App& app)
{
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Processors/")
        .privileges(redfish::privileges::headProcessorCollection)
        .methods(boost::beast::http::verb::head)(
            std::bind_front(handleProcessorCollectionHead, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Processors/")
        .privileges(redfish::privileges::getProcessorCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
        query_param::Query delegated;
        query_param::QueryCapabilities capabilities = {
            .canDelegateExpandLevel = 6,
        };
        if (!redfish::setUpRedfishRouteWithDelegation(app, req, asyncResp,
                                                      delegated, capabilities))
        {
            return;
        }
        if (systemName != "system")
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        asyncResp->res.addHeader(
            boost::beast::http::field::link,
            "</redfish/v1/JsonSchemas/ProcessorCollection/ProcessorCollection.json>; rel=describedby");
        asyncResp->res.jsonValue["@odata.type"] =
            "#ProcessorCollection.ProcessorCollection";
        asyncResp->res.jsonValue["Name"] = "Processor Collection";

        asyncResp->res.jsonValue["@odata.id"] =
            "/redfish/v1/Systems/system/Processors";

        if ((delegated.expandLevel > 0) &&
            delegated.expandType != query_param::ExpandType::None)
        {
            BMCWEB_LOG_DEBUG << "Use efficient processor expand handler";
            getProcessorCollectionWithExpand(asyncResp, delegated.expandLevel);
        }
        else
        {
            BMCWEB_LOG_DEBUG << "Use default processor expand handler";
            collection_util::getCollectionMembers(
            asyncResp,
            boost::urls::url("/redfish/v1/Systems/system/Processors"),
            std::vector<const char*>(processorInterfaces.begin(),
                                     processorInterfaces.end()));
        }
        });
}

inline void requestRoutesProcessor(App& app)
{
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Processors/<str>/")
        .privileges(redfish::privileges::headProcessor)
        .methods(boost::beast::http::verb::head)(
            std::bind_front(handleProcessorHead, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Processors/<str>/")
        .privileges(redfish::privileges::getProcessor)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName,
                   const std::string& processorId) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        if (systemName != "system")
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        getProcessorObject(asyncResp, processorId,
                           std::bind_front(getProcessorData, asyncResp,
                                           ""_json_pointer, 0, processorId));
        });

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Processors/<str>/")
        .privileges(redfish::privileges::patchProcessor)
        .methods(boost::beast::http::verb::patch)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName,
                   const std::string& processorId) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        if (systemName != "system")
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        std::optional<nlohmann::json> appliedConfigJson;
        if (!json_util::readJsonPatch(req, asyncResp->res,
                                      "AppliedOperatingConfig",
                                      appliedConfigJson))
        {
            return;
        }

        std::string appliedConfigUri;
        if (appliedConfigJson)
        {
            if (!json_util::readJson(*appliedConfigJson, asyncResp->res,
                                     "@odata.id", appliedConfigUri))
            {
                return;
            }
            // Check for 404 and find matching D-Bus object, then run
            // property patch handlers if that all succeeds.
            getProcessorObject(asyncResp, processorId,
                               std::bind_front(patchAppliedOperatingConfig,
                                               asyncResp, processorId,
                                               appliedConfigUri));
        }
        });
}

inline void requestRoutesSubProcessorCoreCollection(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/Processors/<str>/SubProcessors")
        .privileges(redfish::privileges::headProcessorCollection)
        .methods(boost::beast::http::verb::head)(std::bind_front(
            handleSubProcessorCoreCollectionHead, std::ref(app)));

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/Processors/<str>/SubProcessors")
        .privileges(redfish::privileges::getProcessorCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName,
                   const std::string& processorId) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        if (systemName != "system")
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        BMCWEB_LOG_DEBUG << "Get available system sub processor core members.";

        getProcessorPaths(asyncResp, processorId,
                          std::bind_front(getSubProcessorCoreMembers, asyncResp,
                                          processorId));
        });
}

inline void requestRoutesSubProcessorCore(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/Processors/<str>/SubProcessors/<str>")
        .privileges(redfish::privileges::headProcessor)
        .methods(boost::beast::http::verb::head)(
            std::bind_front(handleSubProcessorCoreHead, std::ref(app)));

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/Processors/<str>/SubProcessors/<str>")
        .privileges(redfish::privileges::getProcessor)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName,
                   const std::string& processorId, const std::string& coreId) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        if (systemName != "system")
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        BMCWEB_LOG_DEBUG
            << "Get available system sub processor core resources.";

        getProcessorPaths(asyncResp, processorId,
                          std::bind_front(getSubProcessorCoreData, asyncResp,
                                          processorId, coreId));
        });
}

inline void requestRoutesSubProcessorThreadCollection(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/Processors/<str>/SubProcessors/<str>/SubProcessors")
        .privileges(redfish::privileges::headProcessorCollection)
        .methods(boost::beast::http::verb::head)(std::bind_front(
            handleSubProcessorThreadCollectionHead, std::ref(app)));
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/Processors/<str>/SubProcessors/<str>/SubProcessors")
        .privileges(redfish::privileges::getProcessorCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName,
                   const std::string& processorId, const std::string& coreId) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        if (systemName != "system")
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        getSubProcessorCorePaths(asyncResp, processorId, coreId,
                                 std::bind_front(getSubProcessorThreadMembers,
                                                 asyncResp, processorId,
                                                 coreId));
        });
}

inline void requestRoutesSubProcessorThread(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/Processors/<str>/SubProcessors/<str>/SubProcessors/<str>")
        .privileges(redfish::privileges::headProcessor)
        .methods(boost::beast::http::verb::head)(
            std::bind_front(handleSubProcessorThreadHead, std::ref(app)));
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/Processors/<str>/SubProcessors/<str>/SubProcessors/<str>")
        .privileges(redfish::privileges::getProcessor)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName,
                   const std::string& processorId, const std::string& coreId,
                   const std::string& threadId) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        if (systemName != "system")
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        getSubProcessorCorePaths(asyncResp, processorId, coreId,
                                 std::bind_front(getSubProcessorThreadData,
                                                 asyncResp, processorId, coreId,
                                                 threadId));
        });
}

} // namespace redfish
