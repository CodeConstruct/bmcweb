/*
// Copyright (c) 2019 Intel Corporation
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

#include "app.hpp"
#include "dbus_utility.hpp"
#include "health.hpp"
#include "human_sort.hpp"
#include "openbmc_dbus_rest.hpp"
#include "query.hpp"
#include "redfish_util.hpp"
#include "registries/privilege_registry.hpp"
#include "utils/dbus_utils.hpp"

#include <boost/system/error_code.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/unpack_properties.hpp>
#include <utils/location_utils.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_set>

namespace redfish
{

/* Converts a NVMe dbus error to a redfish equivalent, adds to the response */
inline void storageAddDbusError(crow::Response& res, std::string_view func,
                                const std::string& storageId,
                                std::string_view errorName,
                                std::string_view errorDesc)
{
    (void)storageId;

    crow::Response err;

    BMCWEB_LOG_DEBUG << func << " " << errorName << ", " << errorDesc;
    if (errorName == "xyz.openbmc_project.Common.Error.TooManyResources")
    {
        messages::createLimitReachedForResource(err);
    }
    else if (errorName == "xyz.openbmc_project.Common.Error.InvalidArgument")
    {
        messages::propertyValueError(err, "");
    }
    else if (errorName ==
             "xyz.openbmc_project.Common.Error.DeviceOperationFailed")
    {
        messages::operationFailed(err);
    }
    else if (errorName == "xyz.openbmc_project.Common.Error.UnsupportedRequest")
    {
        messages::operationFailed(err);
    }
    else
    {
        messages::internalError(err);
    }

    // Some messages have "error" toplevel, others have "@Message.ExtendedInfo"
    // (addMessageToErrorJson() versus addMessageToJson()). Choose which.
    nlohmann::json extInfo;
    if (err.jsonValue.contains("error"))
    {
        extInfo = err.jsonValue["error"][messages::messageAnnotation][0];
    }
    else
    {
        extInfo = err.jsonValue[messages::messageAnnotation][0];
    }

    // Keep the specific error message provided from the NVMe software.
    extInfo["Message"] = errorDesc;

    messages::moveErrorsToErrorJson(res.jsonValue, extInfo);
    res.result(boost::beast::http::status::bad_request);
}

inline void requestRoutesStorageCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Storage/")
        .privileges(redfish::privileges::getStorageCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
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

        asyncResp->res.jsonValue["@odata.type"] =
            "#StorageCollection.StorageCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            "/redfish/v1/Systems/system/Storage";
        asyncResp->res.jsonValue["Name"] = "Storage Collection";

        constexpr std::array<std::string_view, 1> interface {
            "xyz.openbmc_project.Inventory.Item.Storage"
        };
        collection_util::getCollectionMembers(
            asyncResp,
            crow::utility::urlFromPieces("redfish", "v1", "Systems", "system",
                                         "Storage"),
            interface);
        });

    BMCWEB_ROUTE(app, "/redfish/v1/Storage/")
        .privileges(redfish::privileges::getStorageCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        asyncResp->res.jsonValue["@odata.type"] =
            "#StorageCollection.StorageCollection";
        asyncResp->res.jsonValue["@odata.id"] = "/redfish/v1/Storage";
        asyncResp->res.jsonValue["Name"] = "Storage Collection";
        constexpr std::array<std::string_view, 1> interface {
            "xyz.openbmc_project.Inventory.Item.Storage"
        };
        collection_util::getCollectionMembers(
            asyncResp, crow::utility::urlFromPieces("redfish", "v1", "Storage"),
            interface);
        });
}

inline void getDrives(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::shared_ptr<HealthPopulate>& health,
                      const sdbusplus::message::object_path& storagePath,
                      const std::string& chassisId)
{
    const std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Drive"};
    dbus::utility::getAssociatedSubTreePaths(
        storagePath / "drive",
        sdbusplus::message::object_path("/xyz/openbmc_project/inventory"), 0,
        interfaces,
        [asyncResp, health, chassisId](
            const boost::system::error_code& ec,
            const dbus::utility::MapperGetSubTreePathsResponse& driveList) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "Drive mapper call error";
            messages::internalError(asyncResp->res);
            return;
        }

        nlohmann::json& driveArray = asyncResp->res.jsonValue["Drives"];
        driveArray = nlohmann::json::array();
        auto& count = asyncResp->res.jsonValue["Drives@odata.count"];
        count = 0;

        health->inventory.insert(health->inventory.end(), driveList.begin(),
                                 driveList.end());

        for (const std::string& drive : driveList)
        {
            sdbusplus::message::object_path object(drive);
            if (object.filename().empty())
            {
                BMCWEB_LOG_ERROR << "Failed to find filename in " << drive;
                return;
            }

            nlohmann::json::object_t driveJson;
            driveJson["@odata.id"] = crow::utility::urlFromPieces(
                "redfish", "v1", "Chassis", chassisId, "Drives",
                object.filename());
            driveArray.push_back(std::move(driveJson));
        }

        count = driveArray.size();
        });
}

inline void
    populateWarthogInfo(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        const dbus::utility::MapperServiceMap& ifaces,
                        const std::string& path)
{
    std::string connection = "";
    for (const auto& x : ifaces)
    {
        for (const std::string& y : x.second)
        {
            if (y == "com.google.gbmc.ssd.warthog")
            {
                connection = x.first;
                break;
            }
        }
        if (!connection.empty())
        {
            break;
        }
    }
    if (connection.empty())
    {
        return;
    }

    // Warthog GPIO
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connection, path,
        "com.google.gbmc.ssd.warthog",
        [asyncResp, connection, path](
            const boost::system::error_code ec2,
            const std::vector<std::pair<
                std::string, dbus::utility::DbusVariantType>>& propertiesList) {
        if (ec2)
        {
            // this interface isn't necessary
            return;
        }

        const bool* manufacturingMode = nullptr;
        const bool* pwrseqPgood = nullptr;
        const bool* watchdogTriggered = nullptr;
        const bool* fruEepromWriteProtect = nullptr;
        const bool* morristownOtpWriteProtect = nullptr;
        const bool* triggerPowerCycle = nullptr;
        const bool* triggerReset = nullptr;
        const bool* disableWatchdog = nullptr;
        const bool* debugMode = nullptr;
        const bool* morristownOtpWriteEnable = nullptr;
        const uint64_t* spiImgSelect = nullptr;
        const uint64_t* bootFailureCount = nullptr;
        const std::string* pwrseqState = nullptr;
        const uint64_t* uptimeInSeconds = nullptr;
        const uint64_t* uptimeInMinutes = nullptr;
        const bool* pGoodVdd12V0Ssd = nullptr;
        const bool* pGoodVddPcMor = nullptr;
        const bool* pGoodVdd3V3PcIe = nullptr;
        const bool* pGoodVdd0V83Mor = nullptr;
        const bool* pGoodVttVrefCa = nullptr;
        const bool* pGoodVddFlashVcc = nullptr;
        const bool* pGood12VFlashVpp = nullptr;
        const std::string* cpldVersion = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), propertiesList,
            "ManufacturingMode", manufacturingMode, "WatchdogTriggered",
            watchdogTriggered, "PwrseqPgood", pwrseqPgood,
            "FruEepromWriteProtect", fruEepromWriteProtect,
            "MorristownOtpWriteProtect", morristownOtpWriteProtect,
            "TriggerPowerCycle", triggerPowerCycle, "TriggerReset",
            triggerReset, "DisableWatchdog", disableWatchdog, "DebugMode",
            debugMode, "MorristownOtpWriteEnable", morristownOtpWriteEnable,
            "SpiImgSelect", spiImgSelect, "BootFailureCount", bootFailureCount,
            "PwrseqState", pwrseqState, "UptimeInSeconds", uptimeInSeconds,
            "UptimeInMinutes", uptimeInMinutes, "PGoodVdd12V0Ssd",
            pGoodVdd12V0Ssd, "PGoodVddPcMor", pGoodVddPcMor, "PGoodVdd3V3PcIe",
            pGoodVdd3V3PcIe, "PGoodVdd0V83Mor", pGoodVdd0V83Mor,
            "PGoodVttVrefCa", pGoodVttVrefCa, "PGoodVddFlashVcc",
            pGoodVddFlashVcc, "PGood12VFlashVpp", pGood12VFlashVpp,
            "CpldVersion", cpldVersion);

        if (!success)
        {
            BMCWEB_LOG_CRITICAL << "Failed to parse Warthog Arguments";
            messages::internalError(asyncResp->res);
            return;
        }

        nlohmann::json::object_t warthog;
        warthog["@odata.type"] = "#GoogleWarthog.v1_0_0.GoogleWarthog";
        // Write Only and will always read as false.
        warthog["CpldReset"] = false;

        if (manufacturingMode != nullptr)
        {
            warthog["ManufacturingMode"] = *manufacturingMode;
        }
        if (pwrseqPgood != nullptr)
        {
            warthog["PwrseqPgood"] = *pwrseqPgood;
        }
        if (watchdogTriggered != nullptr)
        {
            warthog["WatchdogTriggered"] = *watchdogTriggered;
        }
        if (fruEepromWriteProtect != nullptr)
        {
            warthog["FruEepromWriteProtect"] = *fruEepromWriteProtect;
        }
        if (morristownOtpWriteProtect != nullptr)
        {
            warthog["MorristownOtpWriteProtect"] = *morristownOtpWriteProtect;
        }
        if (triggerPowerCycle != nullptr)
        {
            warthog["TriggerPowerCycle"] = *triggerPowerCycle;
        }
        if (triggerReset != nullptr)
        {
            warthog["TriggerReset"] = *triggerReset;
        }
        if (disableWatchdog != nullptr)
        {
            warthog["DisableWatchdog"] = *disableWatchdog;
        }
        if (debugMode != nullptr)
        {
            warthog["DebugMode"] = *debugMode;
        }
        if (morristownOtpWriteEnable != nullptr)
        {
            warthog["MorristownOtpWriteEnable"] = *morristownOtpWriteEnable;
        }
        if (spiImgSelect != nullptr)
        {
            warthog["SpiImgSelect"] = *spiImgSelect;
        }
        if (bootFailureCount != nullptr)
        {
            warthog["BootFailureCount"] = *bootFailureCount;
        }
        if (pwrseqState != nullptr)
        {
            warthog["PwrseqState"] = *pwrseqState;
        }
        if (uptimeInSeconds != nullptr)
        {
            warthog["UptimeInSeconds"] = *uptimeInSeconds;
        }
        if (uptimeInMinutes != nullptr)
        {
            warthog["UptimeInMinutes"] = *uptimeInMinutes;
        }
        if (pGoodVdd12V0Ssd != nullptr)
        {
            warthog["PGoodVdd12V0Ssd"] = *pGoodVdd12V0Ssd;
        }
        if (pGoodVddPcMor != nullptr)
        {
            warthog["PGoodVddPcMor"] = *pGoodVddPcMor;
        }
        if (pGoodVdd3V3PcIe != nullptr)
        {
            warthog["PGoodVdd3V3PcIe"] = *pGoodVdd3V3PcIe;
        }
        if (pGoodVdd0V83Mor != nullptr)
        {
            warthog["PGoodVdd0V83Mor"] = *pGoodVdd0V83Mor;
        }
        if (pGoodVttVrefCa != nullptr)
        {
            warthog["PGoodVttVrefCa"] = *pGoodVttVrefCa;
        }
        if (pGoodVddFlashVcc != nullptr)
        {
            warthog["PGoodVddFlashVcc"] = *pGoodVddFlashVcc;
        }
        if (pGood12VFlashVpp != nullptr)
        {
            warthog["PGood12VFlashVpp"] = *pGood12VFlashVpp;
        }
        if (cpldVersion != nullptr)
        {
            warthog["CpldVersion"] = *cpldVersion;
        }

        warthog["Name"] = "Warthog GPIO Action Info";
        asyncResp->res.jsonValue["Links"]["Oem"]["Google"]["Warthog"] =
            std::move(warthog);

        sdbusplus::asio::getAllProperties(
            *crow::connections::systemBus, connection, path,
            "xyz.openbmc_project.Inventory.Decorator.Asset",
            [asyncResp](
                const boost::system::error_code ec3,
                const std::vector<std::pair<
                    std::string, dbus::utility::DbusVariantType>>& asset) {
            if (ec3)
            {
                // this interface isn't necessary
                return;
            }
            nlohmann::json::object_t warthogFruEeprom;

            const std::string* partNumber = nullptr;
            const std::string* serialNumber = nullptr;
            const std::string* manufacturer = nullptr;
            const std::string* model = nullptr;
            const std::string* manufactureDate = nullptr;

            const bool assetSuccess = sdbusplus::unpackPropertiesNoThrow(
                dbus_utils::UnpackErrorPrinter(), asset, "PartNumber",
                partNumber, "SerialNumber", serialNumber, "Manufacturer",
                manufacturer, "Model", model, "ManufactureDate",
                manufactureDate);
            if (!assetSuccess)
            {
                BMCWEB_LOG_CRITICAL << "Failed to parse Warthog Arguments";
                return;
            }
            warthogFruEeprom["DeviceName"] = "Warthog";
            // If we get to this point, then it is enabled.
            warthogFruEeprom["Validity"] = "Enabled";
            if (partNumber != nullptr)
            {
                warthogFruEeprom["BrdPartNumber"] = *partNumber;
            }
            if (serialNumber != nullptr)
            {
                warthogFruEeprom["BrdSerialNumber"] = *serialNumber;
            }
            if (manufacturer != nullptr)
            {
                warthogFruEeprom["BrdMfgName"] = *manufacturer;
            }
            if (model != nullptr)
            {
                warthogFruEeprom["BrdProductName"] = *model;
            }
            if (manufactureDate != nullptr)
            {
                warthogFruEeprom["BrdMfgTime"] = *manufactureDate;
            }
            asyncResp->res
                .jsonValue["Links"]["Oem"]["Google"]["Warthog"]["FruEeprom"] =
                std::move(warthogFruEeprom);
            });
        });
}

inline void
    getDriveFromChassis(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        const std::shared_ptr<HealthPopulate>& health,
                        const sdbusplus::message::object_path& storagePath)
{
    const std::array<std::string_view, 2> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Board",
        "xyz.openbmc_project.Inventory.Item.Chassis"};
    dbus::utility::getAssociatedSubTreePaths(
        storagePath / "chassis",
        sdbusplus::message::object_path("/xyz/openbmc_project/inventory"), 0,
        interfaces,
        [asyncResp, health, storagePath](
            const boost::system::error_code ec,
            const dbus::utility::MapperGetSubTreePathsResponse& chassisList) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "Chassis mapper call error";
            messages::internalError(asyncResp->res);
            return;
        }
        if (chassisList.size() != 1)
        {
            BMCWEB_LOG_ERROR
                << "Storage is not associated with only one chassis";
            messages::internalError(asyncResp->res);
            return;
        }

        std::string chassisPath = chassisList.front();
        std::string chassisId =
            sdbusplus::message::object_path(chassisPath).filename();
        if (chassisId.empty())
        {
            BMCWEB_LOG_ERROR << "Failed to find filename in " << chassisPath;
            return;
        }
        getDrives(asyncResp, health, storagePath, chassisId);
        });
}

inline void requestRoutesStorage(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Storage/<str>/")
        .privileges(redfish::privileges::getStorage)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName,
                   const std::string& storageId) {
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

        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.Inventory.Item.Storage"};
        dbus::utility::getSubTree(
            "/xyz/openbmc_project/inventory", 0, interfaces,
            [asyncResp, storageId](
                const boost::system::error_code ec,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "requestRoutesStorage DBUS response error";
                messages::resourceNotFound(
                    asyncResp->res, "#Storage.v1_13_0.Storage", storageId);
                return;
            }
            auto storage = std::find_if(
                subtree.begin(), subtree.end(),
                [&storageId](
                    const std::pair<std::string,
                                    dbus::utility::MapperServiceMap>& object) {
                return sdbusplus::message::object_path(object.first)
                           .filename() == storageId;
                });
            if (storage == subtree.end())
            {
                messages::resourceNotFound(
                    asyncResp->res, "#Storage.v1_13_0.Storage", storageId);
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] =
                "#Storage.v1_13_0.Storage";
            asyncResp->res.jsonValue["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Systems",
                                             "system", "Storage", storageId);
            asyncResp->res.jsonValue["Name"] = "Storage";
            asyncResp->res.jsonValue["Id"] = storageId;
            asyncResp->res.jsonValue["Status"]["State"] = "Enabled";

            auto health = std::make_shared<HealthPopulate>(asyncResp);
            health->populate();

            getDriveFromChassis(asyncResp, health, storage->first);
            asyncResp->res.jsonValue["Controllers"]["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Systems",
                                             "system", "Storage", storageId,
                                             "Controllers");
            asyncResp->res.jsonValue["Volumes"]["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Systems",
                                             "system", "Storage", storageId,
                                             "Volumes");
            });
        });

    BMCWEB_ROUTE(app, "/redfish/v1/Storage/<str>/")
        .privileges(redfish::privileges::getStorage)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& storageId) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            BMCWEB_LOG_DEBUG << "requestRoutesStorage setUpRedfishRoute failed";
            return;
        }

        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.Inventory.Item.Storage"};
        dbus::utility::getSubTree(
            "/xyz/openbmc_project/inventory", 0, interfaces,
            [asyncResp, storageId](
                const boost::system::error_code ec,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "requestRoutesStorage DBUS response error";
                messages::resourceNotFound(
                    asyncResp->res, "#Storage.v1_13_0.Storage", storageId);
                return;
            }
            auto storage = std::find_if(
                subtree.begin(), subtree.end(),
                [&storageId](
                    const std::pair<std::string,
                                    dbus::utility::MapperServiceMap>& object) {
                return sdbusplus::message::object_path(object.first)
                           .filename() == storageId;
                });
            if (storage == subtree.end())
            {
                messages::resourceNotFound(
                    asyncResp->res, "#Storage.v1_13_0.Storage", storageId);
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] =
                "#Storage.v1_13_0.Storage";
            asyncResp->res.jsonValue["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Storage",
                                             storageId);
            asyncResp->res.jsonValue["Name"] = "Storage";
            asyncResp->res.jsonValue["Id"] = storageId;
            asyncResp->res.jsonValue["Status"]["State"] = "Enabled";

            // Storage subsystem to Stroage link.
            nlohmann::json::array_t storageServices;
            nlohmann::json::object_t storageService;
            storageService["@odata.id"] = crow::utility::urlFromPieces(
                "redfish", "v1", "Systems", "system", "Storage", storageId);
            storageServices.emplace_back(storageService);
            asyncResp->res.jsonValue["Links"]["StorageServices"] =
                std::move(storageServices);
            asyncResp->res.jsonValue["Links"]["StorageServices@odata.count"] =
                1;
            });
        });
}

inline void getDriveAsset(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& connectionName,
                          const std::string& path)
{
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Decorator.Asset",
        [asyncResp](const boost::system::error_code& ec,
                    const std::vector<
                        std::pair<std::string, dbus::utility::DbusVariantType>>&
                        propertiesList) {
        if (ec)
        {
            // this interface isn't necessary
            return;
        }

        const std::string* partNumber = nullptr;
        const std::string* serialNumber = nullptr;
        const std::string* manufacturer = nullptr;
        const std::string* model = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), propertiesList, "PartNumber",
            partNumber, "SerialNumber", serialNumber, "Manufacturer",
            manufacturer, "Model", model);

        if (!success)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        if (partNumber != nullptr)
        {
            asyncResp->res.jsonValue["PartNumber"] = *partNumber;
        }

        if (serialNumber != nullptr)
        {
            asyncResp->res.jsonValue["SerialNumber"] = *serialNumber;
        }

        if (manufacturer != nullptr)
        {
            asyncResp->res.jsonValue["Manufacturer"] = *manufacturer;
        }

        if (model != nullptr)
        {
            asyncResp->res.jsonValue["Model"] = *model;
        }
        });
}

inline void getDrivePresent(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& connectionName,
                            const std::string& path)
{
    sdbusplus::asio::getProperty<bool>(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Item", "Present",
        [asyncResp, path](const boost::system::error_code& ec,
                          const bool isPresent) {
        // this interface isn't necessary, only check it if
        // we get a good return
        if (ec)
        {
            return;
        }

        if (!isPresent)
        {
            asyncResp->res.jsonValue["Status"]["State"] = "Absent";
        }
        });
}

inline void getDriveState(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& connectionName,
                          const std::string& path)
{
    sdbusplus::asio::getProperty<bool>(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.State.Drive", "Rebuilding",
        [asyncResp](const boost::system::error_code& ec, const bool updating) {
        // this interface isn't necessary, only check it
        // if we get a good return
        if (ec)
        {
            return;
        }

        // updating and disabled in the backend shouldn't be
        // able to be set at the same time, so we don't need
        // to check for the race condition of these two
        // calls
        if (updating)
        {
            asyncResp->res.jsonValue["Status"]["State"] = "Updating";
        }
        });
}

inline std::optional<std::string> convertDriveType(const std::string& type)
{
    if (type == "xyz.openbmc_project.Inventory.Item.Drive.DriveType.HDD")
    {
        return "HDD";
    }
    if (type == "xyz.openbmc_project.Inventory.Item.Drive.DriveType.SSD")
    {
        return "SSD";
    }

    return std::nullopt;
}

inline void addResetLinks(nlohmann::json& driveReset,
                          const std::string& driveId,
                          const std::string& chassisId)
{
    driveReset["target"] = crow::utility::urlFromPieces(
        "redfish", "v1", "Chassis", chassisId, "Drives", driveId, "Actions",
        "Drive.Reset");
    driveReset["@Redfish.ActionInfo"] =
        crow::utility::urlFromPieces("redfish", "v1", "Chassis", chassisId,
                                     "Drives", driveId, "ResetActionInfo");
    return;
}

inline std::optional<std::string> convertDriveProtocol(const std::string& proto)
{
    if (proto == "xyz.openbmc_project.Inventory.Item.Drive.DriveProtocol.SAS")
    {
        return "SAS";
    }
    if (proto == "xyz.openbmc_project.Inventory.Item.Drive.DriveProtocol.SATA")
    {
        return "SATA";
    }
    if (proto == "xyz.openbmc_project.Inventory.Item.Drive.DriveProtocol.NVMe")
    {
        return "NVMe";
    }
    if (proto == "xyz.openbmc_project.Inventory.Item.Drive.DriveProtocol.FC")
    {
        return "FC";
    }

    return std::nullopt;
}

inline void
    getDriveItemProperties(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const std::string& driveId,
                           const std::optional<std::string>& chassisId,
                           const std::string& connectionName,
                           const std::string& path, bool hasDriveState)
{
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Item.Drive",
        [asyncResp, driveId, chassisId, hasDriveState](
            const boost::system::error_code& ec,
            const std::vector<std::pair<
                std::string, dbus::utility::DbusVariantType>>& propertiesList) {
        if (ec)
        {
            // this interface isn't required
            return;
        }
        for (const std::pair<std::string, dbus::utility::DbusVariantType>&
                 property : propertiesList)
        {
            const std::string& propertyName = property.first;
            if (propertyName == "Type")
            {
                const std::string* value =
                    std::get_if<std::string>(&property.second);
                if (value == nullptr)
                {
                    // illegal property
                    BMCWEB_LOG_ERROR << "Illegal property: Type";
                    messages::internalError(asyncResp->res);
                    return;
                }

                std::optional<std::string> mediaType = convertDriveType(*value);
                if (!mediaType)
                {
                    BMCWEB_LOG_ERROR << "Unsupported DriveType Interface: "
                                     << *value;
                    messages::internalError(asyncResp->res);
                    return;
                }

                asyncResp->res.jsonValue["MediaType"] = *mediaType;
            }
            else if (propertyName == "Capacity")
            {
                const uint64_t* capacity =
                    std::get_if<uint64_t>(&property.second);
                if (capacity == nullptr)
                {
                    BMCWEB_LOG_ERROR << "Illegal property: Capacity";
                    messages::internalError(asyncResp->res);
                    return;
                }
                if (*capacity == 0)
                {
                    // drive capacity not known
                    continue;
                }

                asyncResp->res.jsonValue["CapacityBytes"] = *capacity;
            }
            else if (propertyName == "Protocol")
            {
                const std::string* value =
                    std::get_if<std::string>(&property.second);
                if (value == nullptr)
                {
                    BMCWEB_LOG_ERROR << "Illegal property: Protocol";
                    messages::internalError(asyncResp->res);
                    return;
                }

                std::optional<std::string> proto = convertDriveProtocol(*value);
                if (!proto)
                {
                    BMCWEB_LOG_ERROR << "Unsupported DrivePrototype Interface: "
                                     << *value;
                    messages::internalError(asyncResp->res);
                    return;
                }
                asyncResp->res.jsonValue["Protocol"] = *proto;
            }
            else if (propertyName == "PredictedMediaLifeLeftPercent")
            {
                const uint8_t* lifeLeft =
                    std::get_if<uint8_t>(&property.second);
                if (lifeLeft == nullptr)
                {
                    BMCWEB_LOG_ERROR
                        << "Illegal property: PredictedMediaLifeLeftPercent";
                    messages::internalError(asyncResp->res);
                    return;
                }
                // 255 means reading the value is not supported
                if (*lifeLeft != 255)
                {
                    asyncResp->res.jsonValue["PredictedMediaLifeLeftPercent"] =
                        *lifeLeft;
                }
            }
            else if (propertyName == "Resettable" && hasDriveState)
            {
                const bool* value = std::get_if<bool>(&property.second);
                // If Resettable flag is not present, its not considered a
                // failure.
                if (value != nullptr && *value && chassisId.has_value())
                {
                    addResetLinks(
                        asyncResp->res.jsonValue["Actions"]["#Drive.Reset"],
                        driveId, *chassisId);
                }
            }
        }
        });
}

inline void getDriveErase(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& chassisId,
                          const std::string& driveName)
{
    auto eraseUrl = crow::utility::urlFromPieces(
        "redfish", "v1", "Chassis", chassisId, "Drives", driveName, "Actions",
        "Drive.SecureErase");
    asyncResp->res.jsonValue["Actions"]["#Drive.SecureErase"]["target"] =
        eraseUrl;
}

static void addAllDriveInfo(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& driveId,
                            const std::string& connectionName,
                            const std::string& path,
                            const std::vector<std::string>& interfaces,
                            const std::string& chassisId)
{
    bool driveInterface = false;
    bool driveStateInterface = false;
    for (const std::string& interface : interfaces)
    {
        if (interface == "xyz.openbmc_project.Inventory.Decorator.Asset")
        {
            getDriveAsset(asyncResp, connectionName, path);
        }
        else if (interface == "xyz.openbmc_project.Inventory.Item")
        {
            getDrivePresent(asyncResp, connectionName, path);
        }
        else if (interface == "xyz.openbmc_project.State.Drive")
        {
            driveStateInterface = true;
            getDriveState(asyncResp, connectionName, path);
        }
        else if (interface == "xyz.openbmc_project.Inventory.Item.Drive")
        {
            driveInterface = true;
        }
        else if (interface == "xyz.openbmc_project.Inventory.Item.DriveErase")
        {
            getDriveErase(asyncResp, chassisId, driveId);
        }
        else if (interface ==
                 "xyz.openbmc_project.Inventory.Decorator.LocationCode")
        {
            location_util::getLocationCode(asyncResp, connectionName, path,
                                           "/PhysicalLocation"_json_pointer);
        }
        else
        {
            std::optional<std::string> locationType =
                location_util::getLocationType(interface);
            if (!locationType)
            {
                BMCWEB_LOG_DEBUG << "getLocationType for Drive failed for "
                                 << interface;
                continue;
            }
            asyncResp->res
                .jsonValue["PhysicalLocation"]["PartLocation"]["LocationType"] =
                *locationType;
        }
    }

    if (driveInterface)
    {
        getDriveItemProperties(asyncResp, driveId, chassisId, connectionName,
                               path, driveStateInterface);
    }
}

/**
 * Chassis drives, this URL will show all the DriveCollection
 * information
 */
inline void chassisDriveCollectionGet(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    // mapper call lambda
    constexpr std::array<std::string_view, 2> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Board",
        "xyz.openbmc_project.Inventory.Item.Chassis"};
    dbus::utility::getSubTree(
        "/xyz/openbmc_project/inventory", 0, interfaces,
        [asyncResp,
         chassisId](const boost::system::error_code& ec,
                    const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec)
        {
            if (ec == boost::system::errc::host_unreachable)
            {
                messages::resourceNotFound(asyncResp->res, "Chassis",
                                           chassisId);
                return;
            }
            messages::internalError(asyncResp->res);
            return;
        }

        // Iterate over all retrieved ObjectPaths.
        for (const auto& [path, connectionNames] : subtree)
        {
            sdbusplus::message::object_path objPath(path);
            if (objPath.filename() != chassisId)
            {
                continue;
            }

            if (connectionNames.empty())
            {
                BMCWEB_LOG_ERROR << "Got 0 Connection names";
                continue;
            }

            asyncResp->res.jsonValue["@odata.type"] =
                "#DriveCollection.DriveCollection";
            asyncResp->res.jsonValue["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Chassis",
                                             chassisId, "Drives");
            asyncResp->res.jsonValue["Name"] = "Drive Collection";

            // Association lambda
            dbus::utility::getAssociationEndPoints(
                path + "/drive",
                [asyncResp,
                 chassisId](const boost::system::error_code& ec3,
                            const dbus::utility::MapperEndPoints& resp) {
                if (ec3)
                {
                    BMCWEB_LOG_ERROR << "Error in chassis Drive association ";
                }
                nlohmann::json& members = asyncResp->res.jsonValue["Members"];
                // important if array is empty
                members = nlohmann::json::array();

                std::vector<std::string> leafNames;
                for (const auto& drive : resp)
                {
                    sdbusplus::message::object_path drivePath(drive);
                    leafNames.push_back(drivePath.filename());
                }

                std::sort(leafNames.begin(), leafNames.end(),
                          AlphanumLess<std::string>());

                for (const auto& leafName : leafNames)
                {
                    nlohmann::json::object_t member;
                    member["@odata.id"] = crow::utility::urlFromPieces(
                        "redfish", "v1", "Chassis", chassisId, "Drives",
                        leafName);
                    members.push_back(std::move(member));
                    // navigation links will be registered in next patch set
                }
                asyncResp->res.jsonValue["Members@odata.count"] = resp.size();
                }); // end association lambda

        } // end Iterate over all retrieved ObjectPaths
        });
}

inline void requestRoutesChassisDrive(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Drives/")
        .privileges(redfish::privileges::getDriveCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(chassisDriveCollectionGet, std::ref(app)));
}

inline void buildDrive(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::string& chassisId,
                       const std::string& driveName,
                       const boost::system::error_code& ec,
                       const dbus::utility::MapperGetSubTreeResponse& subtree)
{
    if (ec)
    {
        BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
        messages::internalError(asyncResp->res);
        return;
    }

    // Iterate over all retrieved ObjectPaths.
    for (const auto& [path, connectionNames] : subtree)
    {
        sdbusplus::message::object_path objPath(path);
        if (objPath.filename() != driveName)
        {
            continue;
        }

        if (connectionNames.empty())
        {
            BMCWEB_LOG_ERROR << "Got 0 Connection names";
            continue;
        }

        asyncResp->res.jsonValue["@odata.id"] = crow::utility::urlFromPieces(
            "redfish", "v1", "Chassis", chassisId, "Drives", driveName);

        asyncResp->res.jsonValue["@odata.type"] = "#Drive.v1_7_0.Drive";
        asyncResp->res.jsonValue["Name"] = driveName;
        asyncResp->res.jsonValue["Id"] = driveName;
        // default it to Enabled
        asyncResp->res.jsonValue["Status"]["State"] = "Enabled";

        nlohmann::json::object_t linkChassisNav;
        linkChassisNav["@odata.id"] =
            crow::utility::urlFromPieces("redfish", "v1", "Chassis", chassisId);
        asyncResp->res.jsonValue["Links"]["Chassis"] = linkChassisNav;

        addAllDriveInfo(asyncResp, driveName, connectionNames[0].first, path,
                        connectionNames[0].second, chassisId);
    }
}

inline void
    matchAndFillDrive(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& chassisId,
                      const std::string& driveName,
                      const std::vector<std::string>& resp)
{
    for (const std::string& drivePath : resp)
    {
        sdbusplus::message::object_path path(drivePath);
        std::string leaf = path.filename();
        if (leaf != driveName)
        {
            continue;
        }
        //  mapper call drive
        constexpr std::array<std::string_view, 1> driveInterface = {
            "xyz.openbmc_project.Inventory.Item.Drive"};
        dbus::utility::getSubTree(
            "/xyz/openbmc_project/inventory", 0, driveInterface,
            [asyncResp, chassisId, driveName](
                const boost::system::error_code& ec,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            buildDrive(asyncResp, chassisId, driveName, ec, subtree);
            });
        return;
    }
    messages::resourceNotFound(asyncResp->res, "#Drive.v1_7_0.Drive",
                               driveName);
}

struct EraseParams
{

    enum Action
    {
        CryptoErase,
        BlockErase,
        Overwrite,
    } action;

    static std::optional<EraseParams>
        parse(const crow::Request& req,
              std::shared_ptr<bmcweb::AsyncResp> asyncResp)
    {
        // Redfish allows sanitizationType to be defaulted, though we don't
        // know a good default at present, leave it mandatory.
        std::string sanitizationType;

        if (!json_util::readJsonAction(req, asyncResp->res, "SanitizationType",
                                       sanitizationType))
        {
            BMCWEB_LOG_DEBUG << "Missing request json parameters";
            return std::nullopt;
        }

        Action action;
        if (sanitizationType == "BlockErase")
        {
            action = Action::BlockErase;
        }
        else if (sanitizationType == "CryptographicErase")
        {
            action = Action::CryptoErase;
        }
        else if (sanitizationType == "Overwrite")
        {
            // Redfish defines an optional "OverwritePasses" parameter, we
            // don't handle that at the moment. If the client passes it, the
            // readJsonAction will fail it.
            action = Action::Overwrite;
        }
        else
        {
            messages::actionParameterValueNotInList(
                asyncResp->res, sanitizationType, "SanitizationType",
                "Drive.SecureErase");
            return std::nullopt;
        }

        return EraseParams{.action = action};
    }

    std::string actionName() const
    {
        switch (action)
        {
            case Action::CryptoErase:
                return "xyz.openbmc_project.Inventory.Item.DriveErase.EraseAction.CryptoErase";
            case Action::BlockErase:
                return "xyz.openbmc_project.Inventory.Item.DriveErase.EraseAction.BlockErase";
            case Action::Overwrite:
                return "xyz.openbmc_project.Inventory.Item.DriveErase.EraseAction.Overwrite";
        }
        return "unreachable";
    }
};

inline void eraseTaskUpdate(bool eraseInProgress,
                            std::shared_ptr<task::TaskData> taskData,
                            const std::string& connectionName,
                            const std::string& drivePath)
{
    if (eraseInProgress)
    {
        // nothing to do
        return;
    }

    // has finished, either success or failure
    taskData->stopMonitor();
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connectionName, drivePath,
        "xyz.openbmc_project.Inventory.Item.DriveErase",
        [taskData](const boost::system::error_code& ec,
                   const std::vector<std::pair<
                       std::string, dbus::utility::DbusVariantType>>& props) {
        if (ec)
        {
            taskData->messages.emplace_back(messages::internalError());
            taskData->state = "Exception";
            taskData->complete(
                std::move(nlohmann::json()),
                boost::beast::http::status::internal_server_error);
            return;
        }

        std::string errorName;
        std::string errorDescription;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), props, "ErrorName", errorName,
            "ErrorDescription", errorDescription);

        if (!success)
        {
            taskData->messages.emplace_back(messages::internalError());
            taskData->state = "Exception";
            taskData->complete(
                std::move(nlohmann::json()),
                boost::beast::http::status::internal_server_error);
            return;
        }

        if (errorName.empty())
        {
            // Erase Success
            taskData->state = "Completed";
            taskData->percentComplete = 100;
            taskData->messages.emplace_back(messages::success());
            taskData->complete();
        }
        else
        {
            // Erase Failed
            bmcweb::AsyncResp resp;
            storageAddDbusError(resp.res, "eraseTaskUpdate", "", errorName,
                                errorDescription);
            for (auto& m :
                 resp.res.jsonValue["error"][messages::messageAnnotation])
            {
                taskData->messages.emplace_back(m);
            }
            taskData->state = "Exception";
            taskData->complete(std::move(resp.res.jsonValue),
                               resp.res.result());
        }
        });
}

inline bool eraseTaskHandler(sdbusplus::message_t& msg,
                             std::shared_ptr<task::TaskData> taskData,
                             const std::string& connectionName,
                             const std::string& drivePath)
{
    dbus::utility::DBusPropertiesMap props;
    std::string iface;
    msg.read(iface, props);

    if (iface != "xyz.openbmc_project.Inventory.Item.DriveErase")
    {
        BMCWEB_LOG_DEBUG << "eraseTaskHandler wrong interface";
        return !task::completed;
    }

    std::optional<bool> inProgress;
    std::optional<double> erasePercentage;
    sdbusplus::unpackPropertiesNoThrow(dbus_utils::UnpackErrorPrinter(), props,
                                       "EraseInProgress", inProgress,
                                       "ErasePercentage", erasePercentage);

    if (erasePercentage)
    {
        BMCWEB_LOG_DEBUG << "eraseTaskHandler update erasePercentage "
                         << *erasePercentage;
        taskData->percentComplete = static_cast<int>(*erasePercentage);
    }

    if (inProgress)
    {
        BMCWEB_LOG_DEBUG << "eraseTaskHandler update iniProgress "
                         << *inProgress;
        eraseTaskUpdate(*inProgress, taskData, connectionName, drivePath);
    }

    // completion is handled asynchronously so always return !completed
    return !task::completed;
}

inline void eraseDrive(const crow::Request& req,
                       const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::string& connectionName,
                       const std::string& drivePath, const EraseParams& params)
{
    crow::connections::systemBus->async_method_call(
        [req, asyncResp, connectionName, drivePath,
         params](const boost::system::error_code ec,
                 const sdbusplus::message_t& msg) {
        // Failure returned from NVMe
        const ::sd_bus_error* sd_err = msg.get_error();
        if (sd_err)
        {
            storageAddDbusError(asyncResp->res, "Drive Erase", "", sd_err->name,
                                sd_err->message);
            return;
        }

        if (ec)
        {
            BMCWEB_LOG_DEBUG << "Erase dbus error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }

        // success, create the async task
        BMCWEB_LOG_DEBUG << "erase started";
        std::shared_ptr<task::TaskData> task = task::TaskData::createTask(
            [connectionName,
             drivePath](const boost::system::error_code& err,
                        sdbusplus::message_t& taskMsg,
                        const std::shared_ptr<task::TaskData>& taskData) {
            if (err)
            {
                // Internal error in property signal callback?
                BMCWEB_LOG_ERROR << drivePath << ": Error in task";
                taskData->messages.emplace_back(messages::internalError());
                taskData->state = "Cancelled";
                return task::completed;
            }

            return eraseTaskHandler(taskMsg, taskData, connectionName,
                                    drivePath);
            },
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',arg0='xyz.openbmc_project.Inventory.Item.DriveErase',"
            "path='" +
                drivePath + "'");

        task->startTimer(std::chrono::minutes(180));
        task->populateResp(asyncResp->res);
        task->payload.emplace(req);

        // Erase may have completed prior to Task watching for signals, so poll
        // once.
        sdbusplus::asio::getProperty<bool>(
            *crow::connections::systemBus, connectionName, drivePath,
            "xyz.openbmc_project.Inventory.Item.DriveErase", "EraseInProgress",
            [task, connectionName,
             drivePath](const boost::system::error_code& ec2, bool inProgress) {
            if (ec2)
            {
                BMCWEB_LOG_DEBUG << "erase poll error: " << ec2;
                return;
            }

            eraseTaskUpdate(inProgress, task, connectionName, drivePath);
            });
        },
        connectionName, drivePath,
        "xyz.openbmc_project.Inventory.Item.DriveErase", "Erase",
        params.actionName());
}

inline void
    matchAndEraseDrive(const crow::Request& req,
                       const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::vector<std::string>& drivePaths,
                       const std::string& driveName, const EraseParams& params)
{
    // Match the driveName
    int found = 0;
    std::string drivePath;
    for (const std::string& d : drivePaths)
    {
        sdbusplus::message::object_path path(d);
        std::string leaf = path.filename();
        if (leaf == driveName)
        {
            found++;
            drivePath = d;
        }
    }

    if (found > 1)
    {
        // Sanity check
        BMCWEB_LOG_DEBUG << "Multiple drives match name " << driveName;
        messages::internalError(asyncResp->res);
        return;
    }
    else if (found == 0)
    {
        messages::resourceNotFound(asyncResp->res, "#Drive.v1_7_0.Drive",
                                   driveName);
        return;
    }

    // Find the connection
    constexpr std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.Inventory.Item.DriveErase"};
    dbus::utility::getDbusObject(
        drivePath, interfaces,
        [req, asyncResp, params,
         drivePath](const boost::system::error_code& ec,
                    const dbus::utility::MapperGetObject& services) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }

        if (services.size() != 1)
        {
            BMCWEB_LOG_DEBUG << "multiple serviceInterfaces entries";
            messages::internalError(asyncResp->res);
            return;
        }
        auto connectionName = services.front().first;

        // Perform the erase
        eraseDrive(req, asyncResp, connectionName, drivePath, params);
        });
}

// Find Chassis with chassisId and the Drives associated to it.
void findChassisDrive(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& chassisId,
                      std::function<void(const boost::system::error_code ec3,
                                         const std::vector<std::string>& resp)>
                          cb)
{
    constexpr std::array<std::string_view, 2> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Board",
        "xyz.openbmc_project.Inventory.Item.Chassis"};
    // mapper call chassis
    dbus::utility::getSubTree(
        "/xyz/openbmc_project/inventory", 0, interfaces,
        [asyncResp, chassisId,
         cb](const boost::system::error_code& ec,
             const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        // Iterate over all retrieved ObjectPaths.
        int found = 0;
        std::string chassisPath;
        for (const auto& [path, connectionNames] : subtree)
        {
            sdbusplus::message::object_path objPath(path);
            if (objPath.filename() != chassisId)
            {
                continue;
            }

            if (connectionNames.empty())
            {
                BMCWEB_LOG_ERROR << "Got 0 Connection names";
                continue;
            }
            found++;
            chassisPath = path;
        }
        if (found > 1)
        {
            BMCWEB_LOG_ERROR << "Multiple chassis match";
            messages::internalError(asyncResp->res);
            return;
        }
        else if (found == 0)
        {
            messages::resourceNotFound(asyncResp->res,
                                       "#Chassis.v1_14_0.Chassis", chassisId);
            return;
        }
        dbus::utility::getAssociationEndPoints(chassisPath + "/drive", cb);
        });
}

inline void
    handleChassisDriveGet(crow::App& app, const crow::Request& req,
                          const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& chassisId,
                          const std::string& driveName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    findChassisDrive(asyncResp, chassisId,
                     [asyncResp, chassisId,
                      driveName](const boost::system::error_code ec,
                                 const std::vector<std::string>& resp) {
        if (ec)
        {
            return; // no drives = no failures
        }
        matchAndFillDrive(asyncResp, chassisId, driveName, resp);
    });
}

inline void handleDriveSecureErase(crow::App& app, const crow::Request& req,
                                   std::shared_ptr<bmcweb::AsyncResp> asyncResp,
                                   const std::string& chassisId,
                                   const std::string& driveName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    auto p = EraseParams::parse(req, asyncResp);
    if (!p)
    {
        return;
    }
    EraseParams params = *p;

    // Find paths of drives associated with the ChassisId
    findChassisDrive(asyncResp, chassisId,
                     [req, asyncResp, chassisId, driveName,
                      params](const boost::system::error_code ec,
                              const std::vector<std::string>& drivePaths) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }
        matchAndEraseDrive(req, asyncResp, drivePaths, driveName, params);
    });
}

/**
 * This URL will show the drive interface for the specific drive in the chassis
 */
inline void requestRoutesChassisDriveName(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Drives/<str>/")
        .privileges(redfish::privileges::getChassis)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleChassisDriveGet, std::ref(app)));

    BMCWEB_ROUTE(
        app, "/redfish/v1/Chassis/<str>/Drives/<str>/Actions/Drive.SecureErase")
        .privileges(redfish::privileges::postDrive)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleDriveSecureErase, std::ref(app)));
}

inline void setResetType(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         const std::string& driveId, const std::string& action,
                         const dbus::utility::MapperGetSubTreeResponse& subtree)
{
    auto driveState =
        std::find_if(subtree.begin(), subtree.end(), [&driveId](auto& object) {
            const sdbusplus::message::object_path path(object.first);
            return path.filename() == driveId;
        });

    if (driveState == subtree.end())
    {
        messages::resourceNotFound(asyncResp->res, "Drive Action", driveId);
        return;
    }

    const std::string& path = driveState->first;
    const std::vector<std::pair<std::string, std::vector<std::string>>>&
        connectionNames = driveState->second;

    if (connectionNames.size() != 1)
    {
        BMCWEB_LOG_ERROR << "Connection size " << connectionNames.size()
                         << ", not equal to 1";
        messages::internalError(asyncResp->res);
        return;
    }

    sdbusplus::asio::setProperty<std::string>(
        *crow::connections::systemBus, connectionNames[0].first, path,
        "xyz.openbmc_project.State.Drive", "RequestedDriveTransition",
        action.c_str(),
        [asyncResp, action](const boost::system::error_code ec) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "[Set] Bad D-Bus request error for " << action
                             << " : " << ec;
            messages::internalError(asyncResp->res);
            return;
        }
        messages::success(asyncResp->res);
        });
}

/**
 * Performs drive reset action.
 *
 * @param[in] asyncResp - Shared pointer for completing asynchronous calls
 * @param[in] driveId   - D-bus filename to identify the Drive
 * @param[in] resetType - Reset type for the Drive
 */
inline void
    performDriveReset(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& driveId,
                      std::optional<std::string> resetType)
{
    std::string action;
    if (!resetType || *resetType == "PowerCycle")
    {
        action = "xyz.openbmc_project.State.Drive.Transition.Powercycle";
    }
    else if (*resetType == "ForceReset")
    {
        action = "xyz.openbmc_project.State.Drive.Transition.Reboot";
    }
    else
    {
        BMCWEB_LOG_DEBUG << "Invalid property value for ResetType: "
                         << *resetType;
        messages::actionParameterNotSupported(asyncResp->res, *resetType,
                                              "ResetType");
        return;
    }

    BMCWEB_LOG_DEBUG << "Reset Drive with " << action;

    constexpr std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.State.Drive"};
    dbus::utility::getSubTree(
        "/xyz/openbmc_project/inventory", 0, interfaces,
        [asyncResp, driveId,
         action](const boost::system::error_code& ec,
                 const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "DBUS response error";
            messages::internalError(asyncResp->res);
            return;
        }
        setResetType(asyncResp, driveId, action, subtree);
        });
}

inline void
    handleChassisDriveReset(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& driveId,
                            std::optional<std::string> resetType,
                            const std::vector<std::string>& drives)
{
    std::unordered_set<std::string> drivesMap(drives.begin(), drives.end());
    constexpr std::array<std::string_view, 2> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Drive",
        "xyz.openbmc_project.State.Drive"};
    dbus::utility::getSubTree(
        "/xyz/openbmc_project/inventory", 0, interfaces,
        [asyncResp, driveId, resetType,
         drivesMap](const boost::system::error_code& ec,
                    const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "Drive mapper call error ";
            messages::internalError(asyncResp->res);
            return;
        }

        auto drive = std::find_if(
            subtree.begin(), subtree.end(),
            [&driveId, &drivesMap](
                const std::pair<std::string, dbus::utility::MapperServiceMap>&
                    object) {
            return sdbusplus::message::object_path(object.first).filename() ==
                       driveId &&
                   drivesMap.contains(object.first);
            });

        if (drive == subtree.end())
        {
            messages::resourceNotFound(asyncResp->res, "Drive Action Reset",
                                       driveId);
            return;
        }

        const std::string& drivePath = drive->first;
        const dbus::utility::MapperServiceMap& driveConnections = drive->second;
        if (driveConnections.size() != 1)
        {
            BMCWEB_LOG_ERROR << "Connection size " << driveConnections.size()
                             << ", not equal to 1";
            messages::internalError(asyncResp->res);
            return;
        }

        bool driveInterface = false;
        bool driveStateInterface = false;
        for (const std::string& interface : driveConnections[0].second)
        {
            if (interface == "xyz.openbmc_project.Inventory.Item.Drive")
            {
                driveInterface = true;
            }
            if (interface == "xyz.openbmc_project.State.Drive")
            {
                driveStateInterface = true;
            }
        }
        if (!driveInterface || !driveStateInterface)
        {
            BMCWEB_LOG_ERROR << "Drive does not have the required interfaces ";
            messages::internalError(asyncResp->res);
            return;
        }

        sdbusplus::asio::getProperty<bool>(
            *crow::connections::systemBus, driveConnections[0].first, drivePath,
            "xyz.openbmc_project.Inventory.Item.Drive", "Resettable",
            [asyncResp, driveId, resetType](
                const boost::system::error_code propEc, bool resettable) {
            if (propEc)
            {
                BMCWEB_LOG_ERROR << "Failed to get resettable property ";
                messages::internalError(asyncResp->res);
                return;
            }
            if (!resettable)
            {
                messages::actionNotSupported(
                    asyncResp->res, "The drive does not support resets.");
                return;
            }
            performDriveReset(asyncResp, driveId, resetType);
            });
        });
}

/**
 * DriveResetAction class supports the POST method for the Reset (reboot)
 * action.
 */
inline void requestDriveResetAction(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Chassis/<str>/Drives/<str>/Actions/Drive.Reset/")
        .privileges(redfish::privileges::postDrive)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& chassisId, const std::string& driveId) {
        BMCWEB_LOG_DEBUG << "Post Drive Reset.";

        nlohmann::json jsonRequest;
        std::optional<std::string> resetType;
        if (json_util::processJsonFromRequest(asyncResp->res, req,
                                              jsonRequest) &&
            !jsonRequest["ResetType"].empty())
        {
            resetType = jsonRequest["ResetType"];
        }

        findChassisDrive(asyncResp, chassisId,
                         [asyncResp, driveId,
                          resetType](const boost::system::error_code ec,
                                     const std::vector<std::string>& drives) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "failed to find drives";
                messages::internalError(asyncResp->res);
                return; // no drives = no failures
            }
            handleChassisDriveReset(asyncResp, driveId, resetType, drives);
        });
        });
}

inline void handleChassisDriveResetActionInfo(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId, const std::string& driveId,
    const std::vector<std::string>& drives)
{
    std::unordered_set<std::string> drivesMap(drives.begin(), drives.end());

    constexpr std::array<std::string_view, 2> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Drive",
        "xyz.openbmc_project.State.Drive"};
    dbus::utility::getSubTree(
        "/xyz/openbmc_project/inventory", 0, interfaces,
        [asyncResp, chassisId, driveId,
         drivesMap](const boost::system::error_code ec,
                    const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "Drive mapper call error";
            messages::internalError(asyncResp->res);
            return;
        }

        auto drive = std::find_if(
            subtree.begin(), subtree.end(),
            [&driveId, &drivesMap](
                const std::pair<std::string,
                                std::vector<std::pair<
                                    std::string, std::vector<std::string>>>>&
                    object) {
            return sdbusplus::message::object_path(object.first).filename() ==
                       driveId &&
                   drivesMap.contains(object.first);
            });

        if (drive == subtree.end())
        {
            messages::resourceNotFound(asyncResp->res, "Drive ResetActionInfo",
                                       driveId);
            return;
        }

        const std::string& drivePath = drive->first;
        const dbus::utility::MapperServiceMap& driveConnections = drive->second;

        if (driveConnections.size() != 1)
        {
            BMCWEB_LOG_ERROR << "Connection size " << driveConnections.size()
                             << ", not equal to 1";
            messages::internalError(asyncResp->res);
            return;
        }

        bool driveInterface = false;
        bool driveStateInterface = false;
        for (const std::string& interface : driveConnections[0].second)
        {
            if (interface == "xyz.openbmc_project.Inventory.Item.Drive")
            {
                driveInterface = true;
            }
            if (interface == "xyz.openbmc_project.State.Drive")
            {
                driveStateInterface = true;
            }
        }
        if (!driveInterface || !driveStateInterface)
        {
            BMCWEB_LOG_ERROR << "Drive does not have the required interfaces ";
            messages::internalError(asyncResp->res);
            return;
        }

        sdbusplus::asio::getProperty<bool>(
            *crow::connections::systemBus, driveConnections[0].first, drivePath,
            "xyz.openbmc_project.Inventory.Item.Drive", "Resettable",
            [asyncResp, chassisId,
             driveId](const boost::system::error_code propEc, bool resettable) {
            if (propEc)
            {
                BMCWEB_LOG_ERROR << "Failed to get resettable property ";
                messages::internalError(asyncResp->res);
                return;
            }
            if (!resettable)
            {
                messages::actionNotSupported(
                    asyncResp->res, "The drive does not support resets.");
                return;
            }
            asyncResp->res.jsonValue["@odata.type"] =
                "#ActionInfo.v1_1_2.ActionInfo";
            asyncResp->res.jsonValue["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Chassis",
                                             chassisId, "Drives", driveId,
                                             "ResetActionInfo");
            asyncResp->res.jsonValue["Name"] = "Reset Action Info";
            asyncResp->res.jsonValue["Id"] = "ResetActionInfo";
            nlohmann::json::array_t parameters;
            nlohmann::json::object_t parameter;
            parameter["Name"] = "ResetType";
            parameter["Required"] = true;
            parameter["DataType"] = "String";
            nlohmann::json::array_t allowableValues;
            allowableValues.emplace_back("PowerCycle");
            allowableValues.emplace_back("ForceRestart");
            parameter["AllowableValues"] = std::move(allowableValues);
            parameters.emplace_back(parameter);
            asyncResp->res.jsonValue["Parameters"] = std::move(parameters);
            });
        });
}

/**
 * DriveResetActionInfo derived class for delivering Drive
 * ResetType AllowableValues using ResetInfo schema.
 */
inline void requestRoutesDriveResetActionInfo(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Drives/<str>/ResetActionInfo/")
        .privileges(redfish::privileges::getActionInfo)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& chassisId, const std::string& driveId) {
        findChassisDrive(asyncResp, chassisId,
                         [asyncResp, chassisId,
                          driveId](const boost::system::error_code ec,
                                   const std::vector<std::string>& drives) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "failed to find drives";
                messages::internalError(asyncResp->res);
                return; // no drives = no failures
            }
            handleChassisDriveResetActionInfo(asyncResp, chassisId, driveId,
                                              drives);
        });
        });
}

inline void getStorageControllerAsset(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const boost::system::error_code& ec,
    const std::vector<std::pair<std::string, dbus::utility::DbusVariantType>>&
        propertiesList)
{
    if (ec)
    {
        // this interface isn't necessary
        BMCWEB_LOG_DEBUG << "Failed to get StorageControllerAsset";
        return;
    }

    const std::string* partNumber = nullptr;
    const std::string* serialNumber = nullptr;
    const std::string* manufacturer = nullptr;
    const std::string* model = nullptr;
    if (!sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), propertiesList, "PartNumber",
            partNumber, "SerialNumber", serialNumber, "Manufacturer",
            manufacturer, "Model", model))
    {
        messages::internalError(asyncResp->res);
        return;
    }

    if (partNumber != nullptr)
    {
        asyncResp->res.jsonValue["PartNumber"] = *partNumber;
    }

    if (serialNumber != nullptr)
    {
        asyncResp->res.jsonValue["SerialNumber"] = *serialNumber;
    }

    if (manufacturer != nullptr)
    {
        asyncResp->res.jsonValue["Manufacturer"] = *manufacturer;
    }

    if (model != nullptr)
    {
        asyncResp->res.jsonValue["Model"] = *model;
    }
}

inline void getStorageControllerLocation(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& service, const std::string& path,
    const std::vector<std::string>& interfaces)
{
    nlohmann::json::json_pointer locationPtr = "/Location"_json_pointer;
    for (const std::string& interface : interfaces)
    {
        if (interface == "xyz.openbmc_project.Inventory.Decorator.LocationCode")
        {
            location_util::getLocationCode(asyncResp, service, path,
                                           locationPtr);
        }
        if (location_util::isConnector(interface))
        {
            std::optional<std::string> locationType =
                location_util::getLocationType(interface);
            if (!locationType)
            {
                BMCWEB_LOG_DEBUG
                    << "getLocationType for StorageController failed for "
                    << interface;
                continue;
            }
            asyncResp->res
                .jsonValue[locationPtr]["PartLocation"]["LocationType"] =
                *locationType;
        }
    }
}

// TODO(matt): could move to dbus_utility.hpp
inline std::optional<std::string>
    matchServiceName(const dbus::utility::MapperServiceMap& allServices,
                     const std::string& matchIface)
{
    int found = 0;
    std::string matchService;
    for (const auto& [service, interfaces] : allServices)
    {
        for (const auto& interface : interfaces)
        {
            if (interface == matchIface)
            {
                matchService = service;
                found++;
            }
        }
    }

    if (found == 1)
    {
        return matchService;
    }
    if (found > 1)
    {
        BMCWEB_LOG_DEBUG << "Failed, multiple service names matched for "
                         << matchIface;
    }
    return {};
}

inline void tryPopulateControllerNvme(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& path, const dbus::utility::MapperServiceMap& ifaces)
{
    if (!matchServiceName(ifaces, "xyz.openbmc_project.NVMe.NVMeAdmin"))
    {
        return;
    }

    auto& nvprop = asyncResp->res.jsonValue["NVMeControllerProperties"];
    // TODO(matt) fetch other properties, don't use hardcoded values
    nvprop["ControllerType"] = "IO";
    nvprop["NVMeVersion"] = "1.4";
    (void)path;
}

inline void tryPopulateControllerSecurity(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const boost::urls::url& controllerUrl,
    const dbus::utility::MapperServiceMap& ifaces)
{
    if (!matchServiceName(
            ifaces,
            "xyz.openbmc_project.Inventory.Item.StorageControllerSecurity"))
    {
        return;
    }

    boost::urls::url sendUrl(controllerUrl);
    crow::utility::appendUrlPieces(sendUrl, "Actions",
                                   "StorageController.SecuritySend");
    boost::urls::url receiveUrl(controllerUrl);
    crow::utility::appendUrlPieces(receiveUrl, "Actions",
                                   "StorageController.SecurityReceive");

    auto& actions = asyncResp->res.jsonValue["Actions"];
    actions["#StorageController.SecuritySend"]["target"] = sendUrl;
    actions["#StorageController.SecurityReceive"]["target"] = receiveUrl;
}

inline void storageCtrlAttachedVolumes(
    const sdbusplus::message::object_path& controllerPath,
    std::function<void(const boost::system::error_code& ec,
                       const std::vector<std::string>& volPaths)>
        cb)
{
    // Get list of attached volumes
    constexpr std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Volume"};
    dbus::utility::getAssociatedSubTreePaths(
        controllerPath / "attaching",
        sdbusplus::message::object_path("/xyz/openbmc_project/inventory"), 0,
        interfaces,
        [cb](const boost::system::error_code& ec,
             const std::vector<std::string>& volPaths) { cb(ec, volPaths); });
}

inline void storageVolumes(
    const sdbusplus::message::object_path& storagePath,
    std::function<void(const boost::system::error_code& ec,
                       const std::vector<std::string>& volPaths)>&& cb)
{
    // Get list of attached volumes
    constexpr std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Volume"};
    dbus::utility::getAssociatedSubTreePaths(
        storagePath / "containing",
        sdbusplus::message::object_path("/xyz/openbmc_project/inventory"), 0,
        interfaces,
        [cb](const boost::system::error_code& ec,
             const std::vector<std::string>& volPaths) { cb(ec, volPaths); });
}

inline void populateStorageControllerAttached(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& path)
{
    storageCtrlAttachedVolumes(
        path, [asyncResp](const boost::system::error_code& ec,
                          const std::vector<std::string>& attached) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "populating attached volumes failed";
                messages::internalError(asyncResp->res);
                return;
            }
            asyncResp->res.jsonValue["Links"]["AttachedVolumes"] = attached;
        });
}

inline void populateStorageController(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& storageId, const std::string& controllerId,
    const std::string& connectionName, const std::string& path,
    const dbus::utility::MapperServiceMap& ifaces,
    const std::vector<std::string>& interfaces)
{
    asyncResp->res.jsonValue["@odata.type"] =
        "#StorageController.v1_7_0.StorageController";
    auto url = crow::utility::urlFromPieces("redfish", "v1", "Systems",
                                            "system", "Storage", storageId,
                                            "Controllers", controllerId);
    asyncResp->res.jsonValue["@odata.id"] = url;
    asyncResp->res.jsonValue["Name"] = controllerId;
    asyncResp->res.jsonValue["Id"] = controllerId;
    asyncResp->res.jsonValue["Status"]["State"] = "Enabled";
    asyncResp->res.jsonValue["PartLocation"]["LocationType"] = "Embedded";
    getStorageControllerLocation(asyncResp, connectionName, path, interfaces);
    populateStorageControllerAttached(asyncResp, path);
    tryPopulateControllerNvme(asyncResp, path, ifaces);
    tryPopulateControllerSecurity(asyncResp, url, ifaces);
    populateWarthogInfo(asyncResp, ifaces, path);

    sdbusplus::asio::getProperty<bool>(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Item", "Present",
        [asyncResp](const boost::system::error_code& ec, bool isPresent) {
        // this interface isn't necessary, only check it
        // if we get a good return
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "Failed to get Present property";
            return;
        }
        if (!isPresent)
        {
            asyncResp->res.jsonValue["Status"]["State"] = "Absent";
        }
        });

    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Decorator.Asset",
        [asyncResp](const boost::system::error_code& ec,
                    const std::vector<
                        std::pair<std::string, dbus::utility::DbusVariantType>>&
                        propertiesList) {
        getStorageControllerAsset(asyncResp, ec, propertiesList);
        });
}

inline void securitySendAction(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& path, const dbus::utility::MapperServiceMap& ifaces,
    uint8_t proto, uint16_t protoSpecific, const std::string& dataBase64)
{
    std::string dataString;
    if (!crow::utility::base64Decode(dataBase64, dataString))
    {
        BMCWEB_LOG_DEBUG << "base data base64decode";
        messages::actionParameterValueFormatError(
            asyncResp->res, "<data>", "Data", "StorageController.SecuritySend");
        return;
    }

    // base64Decode outputs a string not bytes
    std::span<const uint8_t> data(
        reinterpret_cast<const uint8_t*>(dataString.data()), dataString.size());

    auto service = matchServiceName(
        ifaces, "xyz.openbmc_project.Inventory.Item.StorageControllerSecurity");
    if (!service)
    {
        BMCWEB_LOG_DEBUG << "No servicename";
        messages::internalError(asyncResp->res);
        return;
    }

    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code ec,
                    const sdbusplus::message_t& msg) {
        // Failure returned from NVMe
        const ::sd_bus_error* sd_err = msg.get_error();
        if (sd_err)
        {
            messages::generalError(asyncResp->res);
            BMCWEB_LOG_DEBUG << "SecuritySend NVMe error";
            if (sd_err->message)
            {
                BMCWEB_LOG_DEBUG << "Error: " << sd_err->name << " message "
                                 << sd_err->message;
                asyncResp->res.jsonValue["error"]["message"] = sd_err->message;
            }
            return;
        }

        if (ec)
        {
            BMCWEB_LOG_DEBUG << "SecuritySend dbus error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }

        // success
        asyncResp->res.result(boost::beast::http::status::no_content);
        },
        *service, path,
        "xyz.openbmc_project.Inventory.Item.StorageControllerSecurity",
        "SecuritySend", proto, protoSpecific, data);
}

inline void securityReceiveAction(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& path, const dbus::utility::MapperServiceMap& ifaces,
    uint8_t proto, uint16_t protoSpecific, uint32_t transferLength)
{
    auto service = matchServiceName(
        ifaces, "xyz.openbmc_project.Inventory.Item.StorageControllerSecurity");
    if (!service)
    {
        BMCWEB_LOG_DEBUG << "No servicename";
        messages::internalError(asyncResp->res);
        return;
    }

    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code ec,
                    const sdbusplus::message_t& msg,
                    const std::vector<uint8_t>& data) {
        // Failure returned from NVMe
        const ::sd_bus_error* sd_err = msg.get_error();
        if (sd_err)
        {
            messages::generalError(asyncResp->res);
            BMCWEB_LOG_DEBUG << "SecurityReceive NVMe error";
            if (sd_err->message)
            {
                BMCWEB_LOG_DEBUG << "Error: " << sd_err->name << " message "
                                 << sd_err->message;
                asyncResp->res.jsonValue["error"]["message"] = sd_err->message;
            }
            return;
        }

        if (ec)
        {
            BMCWEB_LOG_DEBUG << "SecurityReceive dbus error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }

        // Success
        asyncResp->res.jsonValue["Data"] =
            crow::utility::base64encode(std::string_view(
                reinterpret_cast<const char*>(data.data()), data.size()));
        },
        *service, path,
        "xyz.openbmc_project.Inventory.Item.StorageControllerSecurity",
        "SecurityReceive", proto, protoSpecific, transferLength);
}

// Finds a controller and runs a callback
inline void findStorageController(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& storageId, const std::string& controllerId,
    const std::function<void(const std::string& path,
                             const dbus::utility::MapperServiceMap& ifaces)>&
        cb)
{
    // Find storage
    crow::connections::systemBus->async_method_call(
        [asyncResp, storageId, controllerId,
         cb](const boost::system::error_code ec,
             const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG
                << "requestRoutesStorageController DBUS response error";
            messages::resourceNotFound(
                asyncResp->res, "#StorageController.v1_6_0.StorageController",
                controllerId);
            return;
        }

        auto storage = std::find_if(
            subtree.begin(), subtree.end(),
            [&storageId](
                const std::pair<std::string, dbus::utility::MapperServiceMap>&
                    object) {
            return sdbusplus::message::object_path(object.first).filename() ==
                   storageId;
            });
        if (storage == subtree.end())
        {
            messages::resourceNotFound(asyncResp->res,
                                       "#Storage.v1_9_1.Storage", storageId);
            return;
        }

        // Find controller below the storagePath
        crow::connections::systemBus->async_method_call(
            [asyncResp, storageId, controllerId,
             cb](const boost::system::error_code ec2,
                 const dbus::utility::MapperGetSubTreeResponse& subtree2) {
            if (ec2)
            {
                BMCWEB_LOG_DEBUG
                    << "requestRoutesStorageController DBUS response error"
                    << ec2;
                messages::resourceNotFound(
                    asyncResp->res,
                    "#StorageController.v1_6_0.StorageController",
                    controllerId);
                return;
            }

            auto ctrl = std::find_if(
                subtree2.begin(), subtree2.end(),
                [&controllerId](
                    const std::pair<std::string,
                                    dbus::utility::MapperServiceMap>& object) {
                return sdbusplus::message::object_path(object.first)
                           .filename() == controllerId;
                });
            if (ctrl == subtree2.end())
            {
                messages::resourceNotFound(
                    asyncResp->res,
                    "#StorageController.v1_6_0.StorageController",
                    controllerId);
                return;
            }

            cb(ctrl->first, ctrl->second);
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree", storage->first, 0,
            std::array<const char*, 1>{
                "xyz.openbmc_project.Inventory.Item.StorageController"});
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory", 0,
        std::array<std::string, 1>{
            "xyz.openbmc_project.Inventory.Item.Storage"});
}

inline static void
    setWarthogOemGpio(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& path, const std::string& property,
                      bool value)
{

    sdbusplus::asio::setProperty(
        *crow::connections::systemBus, "com.google.gbmc.ssd", path,
        "com.google.gbmc.ssd.warthog", property, value,
        [asyncResp](const boost::system::error_code ec) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "setWarthogOemGpio D-Bus responses error: "
                             << ec;
            messages::internalError(asyncResp->res);
            return;
        }
        messages::success(asyncResp->res);
        });
}

inline static void
    setWarthogSpiImage(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::string& path, const std::string& property,
                       std::string value)
{

    sdbusplus::asio::setProperty(
        *crow::connections::systemBus, "com.google.gbmc.ssd", path,
        "com.google.gbmc.ssd.warthog", property, value,
        [asyncResp](const boost::system::error_code ec) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "setWarthogOemGpio D-Bus responses error: "
                             << ec;
            messages::internalError(asyncResp->res);
            return;
        }
        messages::success(asyncResp->res);
        });
}

inline void
    storagePatchWarthogOem(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const std::string& storageId,
                           const std::string& controllerId,
                           nlohmann::json& warthogOem)
{
    findStorageController(asyncResp, storageId, controllerId,
                          [asyncResp, storageId, controllerId,
                           warthogOem](const std::string& path,
                                       const dbus::utility::MapperServiceMap&) {
        if (warthogOem.contains("MorristownOtpWriteEnable"))
        {
            setWarthogOemGpio(asyncResp, path, "MorristownOtpWriteEnable",
                              warthogOem["MorristownOtpWriteEnable"]);
        }
        if (warthogOem.contains("TriggerPowerCycle"))
        {
            setWarthogOemGpio(asyncResp, path, "TriggerPowerCycle",
                              warthogOem["TriggerPowerCycle"]);
        }
        if (warthogOem.contains("DisableWatchdog"))
        {
            setWarthogOemGpio(asyncResp, path, "DisableWatchdog",
                              warthogOem["DisableWatchdog"]);
        }
        if (warthogOem.contains("TriggerReset"))
        {
            setWarthogOemGpio(asyncResp, path, "TriggerReset",
                              warthogOem["TriggerReset"]);
        }
        if (warthogOem.contains("CpldReset"))
        {
            setWarthogOemGpio(asyncResp, path, "CpldReset",
                              warthogOem["CpldReset"]);
        }
        if (warthogOem.contains("SpiImgSelect"))
        {
            setWarthogSpiImage(asyncResp, path, "SpiImgSelect",
                               warthogOem["SpiImgSelect"]);
        }
    });
}

// Performs storage attach and detach operations.
// Will be called pseudo-recursively (asio dbus callbacks) to perform
// the operations.
inline void storageApplyAttachDetach(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& connectionName, const std::string& controllerPath,
    std::shared_ptr<std::vector<std::string>> attaches,
    std::shared_ptr<std::vector<std::string>> detaches)
{
    if (!detaches->empty())
    {
        sdbusplus::message::object_path v = detaches->back();
        detaches->pop_back();
        BMCWEB_LOG_DEBUG << "detaching " << v.str << " from " << controllerPath
                         << "\n";
        crow::connections::systemBus->async_method_call(
            [asyncResp, connectionName, controllerPath, attaches,
             detaches](const boost::system::error_code ec,
                       const sdbusplus::message_t& msg) {
            // Failure returned from NVMe
            const ::sd_bus_error* sd_err = msg.get_error();
            if (sd_err)
            {
                // TODO remove "" argument
                storageAddDbusError(asyncResp->res, "detach volume NVMe", "",
                                    sd_err->name, sd_err->message);
                return;
            }

            if (ec)
            {
                BMCWEB_LOG_DEBUG << "detach volume dbus error " << ec;
                messages::internalError(asyncResp->res);
                return;
            }

            // "recurse"
            storageApplyAttachDetach(asyncResp, connectionName, controllerPath,
                                     attaches, detaches);
            },
            connectionName, controllerPath,
            "xyz.openbmc_project.Inventory.Item.StorageController",
            "DetachVolume", v);
        return;
    }

    if (!attaches->empty())
    {
        sdbusplus::message::object_path v = attaches->back();
        attaches->pop_back();
        BMCWEB_LOG_DEBUG << "attaching " << v.str << " to " << controllerPath
                         << "\n";
        crow::connections::systemBus->async_method_call(
            [asyncResp, connectionName, controllerPath, attaches,
             detaches](const boost::system::error_code ec,
                       const sdbusplus::message_t& msg) {
            // Failure returned from NVMe
            const ::sd_bus_error* sd_err = msg.get_error();
            if (sd_err)
            {
                // TODO remove "" argument
                storageAddDbusError(asyncResp->res, "attach volume NVMe", "",
                                    sd_err->name, sd_err->message);
                return;
            }

            if (ec)
            {
                BMCWEB_LOG_DEBUG << "attach volume dbus error " << ec;
                messages::internalError(asyncResp->res);
                return;
            }

            // "recurse"
            storageApplyAttachDetach(asyncResp, connectionName, controllerPath,
                                     attaches, detaches);
            },
            connectionName, controllerPath,
            "xyz.openbmc_project.Inventory.Item.StorageController",
            "AttachVolume", v);
        return;
    }

    // both lists are complete, return success with the controller.
    constexpr std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.Inventory.Item.StorageController"};
    dbus::utility::getDbusObject(
        controllerPath, interfaces,
        [asyncResp, connectionName,
         controllerPath](const boost::system::error_code& ec,
                         const dbus::utility::MapperGetObject& interfaceDict) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "attach volume get controller dbus error "
                             << ec;
            messages::internalError(asyncResp->res);
            return;
        }
        if (interfaceDict.size() != 1)
        {
            BMCWEB_LOG_DEBUG << "attachdetach extra services";
            for (auto x : interfaceDict)
            {
                BMCWEB_LOG_DEBUG << "if " << x.first;
            }
            messages::internalError(asyncResp->res);
        }

        auto c = sdbusplus::message::object_path(controllerPath);
        std::string storageId = c.parent_path().parent_path().filename();
        std::string controllerId = c.filename();
        populateStorageController(asyncResp, storageId, controllerId,
                                  connectionName, controllerPath, interfaceDict,
                                  interfaceDict.front().second);
        });
}

inline void storagePatchAttachedVolumes(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& storageId, const std::string& controllerId,
    std::vector<std::string>& updateVolumeURIs)
{
    // vector of {parsed storageId, URI}
    std::vector<std::pair<std::string, std::string>> updateVolIDs;
    for (auto& u : updateVolumeURIs)
    {
        boost::urls::result<boost::urls::url_view> parsedUrl =
            boost::urls::parse_relative_ref(u);
        if (!parsedUrl)
        {
            BMCWEB_LOG_DEBUG << "bad attached volume URI " << u;
            messages::invalidURI(asyncResp->res, u);
            return;
        }
        std::string urlStorageId;
        std::string volumeId;
        if (!crow::utility::readUrlSegments(
                *parsedUrl, "redfish", "v1", "Systems", "system", "Storage",
                std::ref(urlStorageId), "Volumes", std::ref(volumeId)))
        {
            BMCWEB_LOG_DEBUG << "bad attached volume URI " << u;
            messages::invalidURI(asyncResp->res, u);
            return;
        }

        if (urlStorageId != storageId)
        {
            BMCWEB_LOG_DEBUG << "bad attached volume URI " << u;
            messages::invalidURI(asyncResp->res, u);
            return;
        }

        updateVolIDs.push_back({volumeId, u});
    }

    findStorageController(asyncResp, storageId, controllerId,
                          [asyncResp, updateVolIDs](
                              const std::string& controllerPath,
                              const dbus::utility::MapperServiceMap& ifaces) {
        auto& connectionName = ifaces.front().first;

        // Create dbus paths to update. Elements are {dbus_path, URI}
        std::vector<std::pair<std::string, std::string>> updateVolumes;
        auto storagePath = sdbusplus::message::object_path(controllerPath)
                               .parent_path()
                               .parent_path();
        for (auto& [u, uri] : updateVolIDs)
        {
            updateVolumes.push_back({(storagePath / "volumes" / u).str, uri});
        }
        std::sort(updateVolumes.begin(), updateVolumes.end());

        // Get list of available volumes
        storageVolumes(
            storagePath,
            [asyncResp, updateVolumes, connectionName,
             controllerPath](const boost::system::error_code& ec,
                             const std::vector<std::string>& volPaths) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG
                    << "patch attached volumes list volumes failed";
                messages::internalError(asyncResp->res);
                return;
            }

            for (auto& a : volPaths)
            {
                BMCWEB_LOG_DEBUG << "vol is " << a;
            }

            std::vector<std::string> updatePaths;
            // Early check for bad volume paths
            for (auto& [u, uri] : updateVolumes)
            {
                if (std::find(volPaths.begin(), volPaths.end(), u) ==
                    volPaths.end())
                {
                    BMCWEB_LOG_DEBUG << "patch volume not found " << uri;
                    messages::invalidURI(asyncResp->res, uri);
                    return;
                }
                updatePaths.emplace_back(u);
            }

            // Fetch currently attached volumes
            storageCtrlAttachedVolumes(
                controllerPath,
                [asyncResp, updatePaths, connectionName,
                 controllerPath](const boost::system::error_code& ec2,
                                 const std::vector<std::string>& ex) {
                if (ec2)
                {
                    BMCWEB_LOG_DEBUG
                        << "patch attached volumes list attached failed";
                    messages::internalError(asyncResp->res);
                    return;
                }

                // Find changes
                auto attaches = std::make_shared<std::vector<std::string>>();
                auto detaches = std::make_shared<std::vector<std::string>>();
                std::vector<std::string> existing(ex);
                std::sort(existing.begin(), existing.end());
                std::set_difference(updatePaths.begin(), updatePaths.end(),
                                    existing.begin(), existing.end(),
                                    std::back_inserter(*attaches));

                std::set_difference(existing.begin(), existing.end(),
                                    updatePaths.begin(), updatePaths.end(),
                                    std::back_inserter(*detaches));

                // Apply
                storageApplyAttachDetach(asyncResp, connectionName,
                                         controllerPath, attaches, detaches);
                });
            });
    });
}

inline void
    storagePatchController(App& app, const crow::Request& req,
                           const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const std::string& systemName,
                           const std::string& storageId,
                           const std::string& controllerId)
{
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

    std::optional<nlohmann::json> warthogOem;
    std::optional<std::vector<std::string>> attachedVolumes;
    if (!json_util::readJsonPatch(req, asyncResp->res, "Links/AttachedVolumes",
                                  attachedVolumes, "Links/Oem/Google/Warthog",
                                  warthogOem))
    {
        BMCWEB_LOG_DEBUG << "Bad controller patch input";
        return;
    }

    if (warthogOem && attachedVolumes)
    {
        BMCWEB_LOG_DEBUG << "Multiple values to controller patch";
        messages::generalError(asyncResp->res);
        asyncResp->res.jsonValue["error"]["message"] =
            "PATCH may only alter one resource type";
        return;
    }

    if (!(warthogOem || attachedVolumes))
    {
        BMCWEB_LOG_DEBUG << "No values to controller patch";
        messages::noOperation(asyncResp->res);
        return;
    }

    if (warthogOem)
    {
        storagePatchWarthogOem(asyncResp, storageId, controllerId, *warthogOem);
    }

    if (attachedVolumes)
    {
        storagePatchAttachedVolumes(asyncResp, storageId, controllerId,
                                    *attachedVolumes);
    }

    // TODO: we should setCompleteRequestHandler to return the modified
    // StorageController on completion, rather than handling in attachedVolumes.
}

inline void requestRoutesStorageControllerActions(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/Storage/<str>/Controllers/<str>/Actions/StorageController.SecuritySend")
        .privileges(redfish::privileges::postStorageController)
        .methods(boost::beast::http::verb::post)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName, const std::string& storageId,
                   const std::string& controllerId) {
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

        uint8_t proto;
        uint16_t protoSpecific;
        std::string dataBase64;

        if (!json_util::readJsonAction(req, asyncResp->res, "SecurityProtocol",
                                       proto, "SecurityProtocolSpecific",
                                       protoSpecific, "Data", dataBase64))
        {
            BMCWEB_LOG_DEBUG << "Missing request json parameters";
            return;
        }

        findStorageController(
            asyncResp, storageId, controllerId,
            [asyncResp, proto, protoSpecific,
             dataBase64](const std::string& path,
                         const dbus::utility::MapperServiceMap& ifaces) {
            securitySendAction(asyncResp, path, ifaces, proto, protoSpecific,
                               dataBase64);
            });
        });

    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/Storage/<str>/Controllers/<str>/Actions/StorageController.SecurityReceive")
        .privileges(redfish::privileges::postStorageController)
        .methods(boost::beast::http::verb::post)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName, const std::string& storageId,
                   const std::string& controllerId) {
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

        uint8_t proto;
        uint16_t protoSpecific;
        uint32_t transferLength;

        if (!json_util::readJsonAction(req, asyncResp->res, "SecurityProtocol",
                                       proto, "SecurityProtocolSpecific",
                                       protoSpecific, "AllocationLength",
                                       transferLength))
        {
            BMCWEB_LOG_DEBUG << "Missing request json parameters";
            return;
        }

        findStorageController(
            asyncResp, storageId, controllerId,
            [asyncResp, proto, protoSpecific,
             transferLength](const std::string& path,
                             const dbus::utility::MapperServiceMap& ifaces) {
            securityReceiveAction(asyncResp, path, ifaces, proto, protoSpecific,
                                  transferLength);
            });
        });

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/Storage/<str>/Controllers/<str>")
        .privileges(redfish::privileges::patchStorageController)
        .methods(boost::beast::http::verb::patch)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName, const std::string& storageId,
                   const std::string& controllerId) {
        storagePatchController(app, req, asyncResp, systemName, storageId,
                               controllerId);
        });
}

inline void getStorageControllerHandler(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& storageId, const std::string& controllerId,
    const boost::system::error_code& ec,
    const dbus::utility::MapperGetSubTreeResponse& subtree)
{
    if (ec || subtree.empty())
    {
        // doesn't have to be there
        BMCWEB_LOG_DEBUG << "Failed to handle StorageController";
        return;
    }

    for (const auto& [path, interfaceDict] : subtree)
    {
        sdbusplus::message::object_path object(path);
        std::string id = object.filename();
        if (id.empty())
        {
            BMCWEB_LOG_ERROR << "Failed to find filename in " << path;
            return;
        }
        if (id != controllerId)
        {
            continue;
        }

        if (interfaceDict.size() != 1)
        {
            BMCWEB_LOG_ERROR << "Connection size " << interfaceDict.size()
                             << ", greater than 1";
            messages::internalError(asyncResp->res);
            return;
        }

        const std::string& connectionName = interfaceDict.front().first;
        populateStorageController(asyncResp, storageId, controllerId,
                                  connectionName, path, interfaceDict,
                                  interfaceDict.front().second);
    }
}

inline void populateStorageControllerCollection(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const boost::system::error_code& ec, const std::string& storageId,
    const dbus::utility::MapperGetSubTreePathsResponse& controllerList)
{
    nlohmann::json::array_t members;
    if (ec || controllerList.empty())
    {
        asyncResp->res.jsonValue["Members"] = std::move(members);
        asyncResp->res.jsonValue["Members@odata.count"] = 0;
        BMCWEB_LOG_DEBUG << "Failed to find any StorageController";
        return;
    }

    for (const std::string& path : controllerList)
    {
        std::string id = sdbusplus::message::object_path(path).filename();
        if (id.empty())
        {
            BMCWEB_LOG_ERROR << "Failed to find filename in " << path;
            return;
        }
        nlohmann::json::object_t member;
        member["@odata.id"] = crow::utility::urlFromPieces(
            "redfish", "v1", "Systems", "system", "Storage", storageId,
            "Controllers", id);
        members.emplace_back(member);
    }
    asyncResp->res.jsonValue["Members@odata.count"] = members.size();
    asyncResp->res.jsonValue["Members"] = std::move(members);
}

inline void findStorage(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& storageId,
    std::function<void(const sdbusplus::message::object_path& storagePath,
                       const std::string& service)>
        cb)
{
    constexpr std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Storage"};
    // mapper call chassis
    dbus::utility::getSubTree(
        "/xyz/openbmc_project/inventory", 0, interfaces,
        [asyncResp, storageId,
         cb](const boost::system::error_code& ec,
             const dbus::utility::MapperGetSubTreeResponse& storageList) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "findStorage DBUS response error";
            messages::resourceNotFound(asyncResp->res,
                                       "#Storage.v1_13_0.Storage", storageId);
            return;
        }

        auto storage = std::find_if(storageList.begin(), storageList.end(),
                                    [&storageId](auto& entry) {
            const std::string& path = entry.first;
            return sdbusplus::message::object_path(path).filename() ==
                   storageId;
        });
        if (storage == storageList.end())
        {
            BMCWEB_LOG_DEBUG << "findStorage couldn't find " << storageId;
            messages::resourceNotFound(asyncResp->res,
                                       "#Storage.v1_13_0.Storage", storageId);
            return;
        }
        const std::string& storagePath = storage->first;

        const auto& serviceMap = storage->second;
        if (serviceMap.size() != 1)
        {
            BMCWEB_LOG_DEBUG << "findStorage multiple services for storage";
            messages::resourceNotFound(asyncResp->res,
                                       "#Storage.v1_13_0.Storage", storageId);
        }
        const std::string& serviceName = serviceMap.front().first;

        cb(sdbusplus::message::object_path(storagePath), serviceName);
        });
}

inline void findStorage(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& storageId,
    std::function<void(const sdbusplus::message::object_path& storagePath)> cb)
{
    findStorage(asyncResp, storageId,
                [cb](const sdbusplus::message::object_path& storagePath,
                     const std::string& service) {
        (void)service;
        cb(storagePath);
    });
}

inline void storageControllerCollectionHandler(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName, const std::string& storageId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        BMCWEB_LOG_DEBUG
            << "Failed to setup Redfish Route for StorageController Collection";
        return;
    }
    if (systemName != "system")
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        BMCWEB_LOG_DEBUG << "Failed to find ComputerSystem of " << systemName;
        return;
    }

    findStorage(asyncResp, storageId,
                [asyncResp, storageId](
                    const sdbusplus::message::object_path& storagePath) {
        asyncResp->res.jsonValue["@odata.type"] =
            "#StorageControllerCollection.StorageControllerCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            crow::utility::urlFromPieces("redfish", "v1", "Systems", "system",
                                         "Storage", storageId, "Controllers");
        asyncResp->res.jsonValue["Name"] = "Storage Controller Collection";

        auto& cap = asyncResp->res.jsonValue["@Redfish.CollectionCapabilities"];
        cap["@odata.type"] =
            "#CollectionCapabilities.v1_3_0.CollectionCapabilities";
        auto& cs = cap["Capabilities"];
        if (!cs.is_array())
        {
            cs = nlohmann::json::array_t();
        }
        auto& c = cs.emplace_back(nlohmann::json::object_t());
        c["CapabilitiesObject"]["@odata.id"] = crow::utility::urlFromPieces(
            "redfish", "v1", "Systems", "system", "Storage", storageId,
            "Volumes", "Capabilities");
        c["Links"]["TargetCollection"]["@odata.id"] =
            asyncResp->res.jsonValue["@odata.id"];

        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.Inventory.Item.StorageController"};
        dbus::utility::getAssociatedSubTreePaths(
            storagePath / "storage_controller",
            sdbusplus::message::object_path("/xyz/openbmc_project/inventory"),
            0, interfaces,
            [asyncResp,
             storageId](const boost::system::error_code& ec,
                        const dbus::utility::MapperGetSubTreePathsResponse&
                            controllerList) {
            populateStorageControllerCollection(asyncResp, ec, storageId,
                                                controllerList);
            });
    });
}

inline void
    tryPopulateVolumeNvme(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& connectionName,
                          const std::string& path,
                          const dbus::utility::MapperServiceMap& ifaces,
                          const std::string& volumeId, size_t blockSize)
{
    if (!matchServiceName(ifaces, "xyz.openbmc_project.Nvme.Volume"))
    {
        return;
    }

    asyncResp->res.jsonValue["Name"] = std::string("Namespace ") + volumeId;

    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Nvme.Volume",
        [asyncResp, blockSize](
            const boost::system::error_code& ec,
            const std::vector<std::pair<
                std::string, dbus::utility::DbusVariantType>>& propertiesList) {
        if (ec)
        {
            std::cerr << "error fetching nvme volume " << ec << std::endl;
            // this interface isn't necessary
            return;
        }

        const uint32_t* namespaceId = nullptr;
        const size_t* lbaFormat = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), propertiesList, "NamespaceId",
            namespaceId, "LBAFormat", lbaFormat);

        if (!success)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        auto& nvprop = asyncResp->res.jsonValue["NVMeNamespaceProperties"];
        if (namespaceId)
        {
            nvprop["NamespaceId"] =
                std::string("0x") + intToHexString(*namespaceId, 8);
        }
        if (lbaFormat)
        {
            auto& lbafprop = nvprop["LBAFormat"];
            lbafprop["LBAFormatType"] =
                std::string("LBAFormat") + std::to_string(*lbaFormat);
            lbafprop["LBADataSizeBytes"] = blockSize;
            // TODO: populate other lbaformat attributes, and metadata_at_end
        }
        });
}

inline void populateStorageVolume(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& storageId, const std::string& volumeId,
    const std::string& connectionName, const std::string& path,
    const dbus::utility::MapperServiceMap& ifaces)
{
    asyncResp->res.jsonValue["@odata.type"] = "#Volume.v1_9_0.Volume";
    auto url =
        crow::utility::urlFromPieces("redfish", "v1", "Systems", "system",
                                     "Storage", storageId, "Volumes", volumeId);
    asyncResp->res.jsonValue["@odata.id"] = url;
    // May be overridden by nvme
    asyncResp->res.jsonValue["Name"] = std::string("Volume ") + volumeId;
    asyncResp->res.jsonValue["Id"] = volumeId;

    size_t volBlockSize = 0;

    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Item.Volume",
        [asyncResp, &volBlockSize](
            const boost::system::error_code& ec,
            const std::vector<std::pair<
                std::string, dbus::utility::DbusVariantType>>& propertiesList) {
        if (ec)
        {
            // this interface isn't necessary
            return;
        }

        const uint64_t* size;
        const size_t* blockSize;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), propertiesList, "Size", size,
            "BlockSize", blockSize);

        if (!success)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        auto& cap = asyncResp->res.jsonValue["Capacity"];
        auto& capdata = cap["Data"];
        if (size)
        {
            capdata["ProvisionedBytes"] = *size;
        }
        // Capacity.Metadata or provisioned/allocated is not currently handled
        // by OpenBMC
        if (blockSize)
        {
            asyncResp->res.jsonValue["BlockSizeBytes"] = *blockSize;
            volBlockSize = *blockSize;
        }
        });

    tryPopulateVolumeNvme(asyncResp, connectionName, path, ifaces, volumeId,
                          volBlockSize);
}

inline void
    deleteStorageVolume(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        const std::string& storageId,
                        const std::string& connectionName,
                        const std::string& path)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp, storageId](const boost::system::error_code ec,
                               const sdbusplus::message_t& msg) {
        // Failure returned from NVMe
        const ::sd_bus_error* sd_err = msg.get_error();
        if (sd_err)
        {
            storageAddDbusError(asyncResp->res, "delete Volume NVMe", storageId,
                                sd_err->name, sd_err->message);
            return;
        }

        if (ec)
        {
            BMCWEB_LOG_DEBUG << "delete Volume dbus error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }

        // success
        asyncResp->res.result(boost::beast::http::status::no_content);
        },
        connectionName, path, "xyz.openbmc_project.Object.Delete", "Delete");
}

inline void findStorageVolume(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& storageId, const std::string& volumeId,
    const std::function<
        void(const std::string& path, const std::string& connectionName,
             const dbus::utility::MapperServiceMap& ifaces)>& cb)
{
    findStorage(asyncResp, storageId,
                [asyncResp, storageId, volumeId,
                 cb](const sdbusplus::message::object_path& storagePath) {
        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.Inventory.Item.Volume"};
        dbus::utility::getAssociatedSubTree(
            storagePath / "containing",
            sdbusplus::message::object_path("/xyz/openbmc_project/inventory"),
            0, interfaces,
            [asyncResp, storageId, volumeId,
             cb](const boost::system::error_code& ec,
                 const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec || subtree.empty())
            {
                BMCWEB_LOG_DEBUG << "findStorageVolume error" << ec;
                messages::resourceNotFound(asyncResp->res,
                                           "#Volume.v1_9_0.Volume", volumeId);
                return;
            }

            for (const auto& [path, interfaceDict] : subtree)
            {
                sdbusplus::message::object_path object(path);
                std::string id = object.filename();
                if (id.empty())
                {
                    BMCWEB_LOG_ERROR << "Failed to find filename in " << path;
                    messages::resourceNotFound(
                        asyncResp->res, "#Volume.v1_9_0.Volume", volumeId);
                    return;
                }
                if (id != volumeId)
                {
                    continue;
                }

                if (interfaceDict.size() != 1)
                {
                    BMCWEB_LOG_ERROR << "Connection size "
                                     << interfaceDict.size()
                                     << ", greater than 1";
                    messages::internalError(asyncResp->res);
                    return;
                }

                const std::string& connectionName = interfaceDict.front().first;
                cb(path, connectionName, interfaceDict);
                return;
            }
            BMCWEB_LOG_DEBUG << "findStorageVolume not found";
            messages::resourceNotFound(asyncResp->res, "#Volume.v1_9_0.Volume",
                                       volumeId);
            });
    });
}

inline void createVolumeSuccess(std::shared_ptr<task::TaskData> taskData,
                                const std::string& service,
                                const std::string& storageId,
                                const std::string& progressPath)
{
    taskData->stopMonitor();

    sdbusplus::asio::getProperty<sdbusplus::message::object_path>(
        *crow::connections::systemBus, service, progressPath,
        "xyz.openbmc_project.Nvme.CreateVolumeProgressSuccess", "VolumePath",
        [taskData, storageId,
         progressPath](const boost::system::error_code& ec,
                       const sdbusplus::message::object_path& volumePath) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "createVolumeSuccess volumepath error " << ec;
            taskData->messages.emplace_back(messages::internalError());
            taskData->state = "Exception";
            taskData->complete(
                std::move(nlohmann::json()),
                boost::beast::http::status::internal_server_error);
            return;
        }

        auto resp = std::make_shared<bmcweb::AsyncResp>();
        resp->res.setCompleteRequestHandler([taskData](crow::Response& res) {
            if (res.result() == boost::beast::http::status::ok)
            {
                taskData->messages.emplace_back(messages::created());
                taskData->state = "Completed";
                taskData->complete(std::move(res.jsonValue),
                                   boost::beast::http::status::created);
            }
            else
            {
                BMCWEB_LOG_DEBUG << "createVolumeSuccess error populating: "
                                 << res.result();
                BMCWEB_LOG_DEBUG << res.jsonValue;
                taskData->messages.emplace_back(messages::internalError());
                taskData->state = "Exception";
                taskData->complete(
                    std::move(nlohmann::json()),
                    boost::beast::http::status::internal_server_error);
            }
        });

        auto volumeId = volumePath.filename();

        findStorageVolume(resp, storageId, volumeId,
                          [resp, storageId, volumeId](
                              const std::string& path,
                              const std::string& connectionName,
                              const dbus::utility::MapperServiceMap& ifaces) {
            BMCWEB_LOG_DEBUG << "createVolumeSuccess connectionName is "
                             << connectionName;
            populateStorageVolume(resp, storageId, volumeId, connectionName,
                                  path, ifaces);
            // on completion completeRequestHandler above will copy the response
            // to taskData
        });
        });
}

inline void createVolumeFailure(std::shared_ptr<task::TaskData> taskData,
                                const std::string& service,
                                const std::string& storageId,
                                const std::string& progressPath)
{
    taskData->stopMonitor();

    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, service, progressPath,
        "xyz.openbmc_project.Nvme.CreateVolumeProgressFailure",
        [taskData,
         storageId](const boost::system::error_code& ec,
                    const std::vector<std::pair<
                        std::string, dbus::utility::DbusVariantType>>& props) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "createVolumeSuccess volumepath error " << ec;
            taskData->messages.emplace_back(messages::internalError());
            taskData->state = "Exception";
            taskData->complete(
                std::move(nlohmann::json()),
                boost::beast::http::status::internal_server_error);
            return;
        }

        std::string errorName;
        std::string errorDesc;
        sdbusplus::unpackPropertiesNoThrow(dbus_utils::UnpackErrorPrinter(),
                                           props, "ErrorName", errorName,
                                           "ErrorDescription", errorDesc);
        bmcweb::AsyncResp resp;
        storageAddDbusError(resp.res, "createVolumeFailure", storageId,
                            errorName, errorDesc);
        for (auto& m : resp.res.jsonValue["error"][messages::messageAnnotation])
        {
            taskData->messages.emplace_back(m);
        }

        taskData->state = "Exception";
        taskData->complete(std::move(resp.res.jsonValue), resp.res.result());
        });
}

// Handles the Status property of Common.Progress interface
inline void createVolumeTaskUpdate(const std::string& status,
                                   std::shared_ptr<task::TaskData> taskData,
                                   const std::string& service,
                                   const std::string& storageId,
                                   const std::string& progressPath)
{
    if (status ==
        "xyz.openbmc_project.Common.Progress.OperationStatus.InProgress")
    {
        // nothing to do
    }
    else if (status ==
             "xyz.openbmc_project.Common.Progress.OperationStatus.Completed")
    {
        createVolumeSuccess(taskData, service, storageId, progressPath);
    }
    else if (status ==
                 "xyz.openbmc_project.Common.Progress.OperationStatus.Failed" ||
             status ==
                 "xyz.openbmc_project.Common.Progress.OperationStatus.Aborted")
    {
        createVolumeFailure(taskData, service, storageId, progressPath);
    }
    else
    {
        BMCWEB_LOG_DEBUG << "updateCreateVolumeTask unexpected state "
                         << status;
    }
}

// Handler called by TaskData on Commmon.Progress property change
inline bool createVolumeTaskHandler(sdbusplus::message_t& msg,
                                    std::shared_ptr<task::TaskData> taskData,
                                    const std::string& service,
                                    const std::string& storageId,
                                    const std::string& progressPath)
{
    dbus::utility::DBusPropertiesMap props;
    std::string iface;
    msg.read(iface, props);

    if (iface != "xyz.openbmc_project.Common.Progress")
    {
        BMCWEB_LOG_DEBUG << "updateCreateVolumeTask wrong interface";
        return !task::completed;
    }

    std::optional<std::string> status;
    sdbusplus::unpackPropertiesNoThrow(dbus_utils::UnpackErrorPrinter(), props,
                                       "Status", status);
    if (!status)
    {
        BMCWEB_LOG_DEBUG << "updateCreateVolumeTask not status update";
        return !task::completed;
    }

    createVolumeTaskUpdate(*status, taskData, service, storageId, progressPath);
    // completion is handled asynchronously so always return !completed
    return !task::completed;
}

inline void
    createStorageVolume(const crow::Request& req,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        const std::string& storagePath,
                        const std::string& storageService, uint64_t size,
                        size_t lbaIndex, bool metadataAtEnd)
{
    auto storageId = sdbusplus::message::object_path(storagePath).filename();
    crow::connections::systemBus->async_method_call(
        [req, asyncResp, storageId, storageService](
            const boost::system::error_code ec, const sdbusplus::message_t& msg,
            const sdbusplus::message::object_path& progressPath) {
        const ::sd_bus_error* sd_err = msg.get_error();
        if (sd_err)
        {
            storageAddDbusError(asyncResp->res, "create Volume NVMe", storageId,
                                sd_err->name, sd_err->message);
            return;
        }

        if (ec)
        {
            BMCWEB_LOG_DEBUG << "create Volume dbus error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }

        // success
        BMCWEB_LOG_DEBUG << "create volume success, progress path "
                         << progressPath.str;
        std::shared_ptr<task::TaskData> task = task::TaskData::createTask(
            [storageService, storageId,
             progressPath](const boost::system::error_code& err,
                           sdbusplus::message_t& taskMsg,
                           const std::shared_ptr<task::TaskData>& taskData) {
            if (err)
            {
                // Internal error in property signal callback?
                BMCWEB_LOG_ERROR << progressPath.str << ": Error in task";
                taskData->messages.emplace_back(messages::internalError());
                taskData->state = "Cancelled";
                return task::completed;
            }

            return createVolumeTaskHandler(taskMsg, taskData, storageService,
                                           storageId, progressPath);
            },
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',arg0='xyz.openbmc_project.Common.Progress',"
            "path='" +
                progressPath.str + "'");

        task->startTimer(std::chrono::minutes(60));
        task->populateResp(asyncResp->res);
        task->payload.emplace(req);

        // Progress may have completed prior to Task watching for signals, so
        // poll Status once.
        sdbusplus::asio::getProperty<sdbusplus::message::object_path>(
            *crow::connections::systemBus, storageService, progressPath,
            "xyz.openbmc_project.Common.Progress", "Status",
            [task, storageService, storageId,
             progressPath](const boost::system::error_code& ec2,
                           const sdbusplus::message::object_path& status) {
            if (ec2)
            {
                BMCWEB_LOG_DEBUG << "createVolume poll error: " << ec2;
                return;
            }

            createVolumeTaskUpdate(status.str, task, storageService, storageId,
                                   progressPath);
            });
        },
        storageService, storagePath, "xyz.openbmc_project.Nvme.Storage",
        "CreateVolume", size, lbaIndex, metadataAtEnd);
}

inline void populateStorageVolumeCollection(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const boost::system::error_code& ec, const std::string& storageId,
    const dbus::utility::MapperGetSubTreePathsResponse& volumeList)
{
    nlohmann::json::array_t members;
    if (ec || volumeList.empty())
    {
        asyncResp->res.jsonValue["Members"] = std::move(members);
        asyncResp->res.jsonValue["Members@odata.count"] = 0;
        BMCWEB_LOG_DEBUG << "Failed to find any storage Volumes";
        return;
    }

    for (const std::string& path : volumeList)
    {
        std::string id = sdbusplus::message::object_path(path).filename();
        if (id.empty())
        {
            BMCWEB_LOG_ERROR << "Failed to find filename in " << path;
            return;
        }
        nlohmann::json::object_t member;
        member["@odata.id"] =
            crow::utility::urlFromPieces("redfish", "v1", "Systems", "system",
                                         "Storage", storageId, "Volumes", id);
        members.emplace_back(member);
    }
    asyncResp->res.jsonValue["Members@odata.count"] = members.size();
    asyncResp->res.jsonValue["Members"] = std::move(members);
}

inline void
    storageVolumeHandler(App& app, const crow::Request& req,
                         const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         const std::string& systemName,
                         const std::string& storageId,
                         const std::string& volumeId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        BMCWEB_LOG_DEBUG << "Failed to setup Redfish Route for StorageVolume";
        return;
    }
    if (systemName != "system")
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        BMCWEB_LOG_DEBUG << "Failed to find ComputerSystem of " << systemName;
        return;
    }
    findStorageVolume(
        asyncResp, storageId, volumeId,
        [asyncResp, storageId,
         volumeId](const std::string& path, const std::string& connectionName,
                   const dbus::utility::MapperServiceMap& ifaces) {
        populateStorageVolume(asyncResp, storageId, volumeId, connectionName,
                              path, ifaces);
        });
}

std::optional<size_t> parseLbaFormatType(std::string_view ty)
{
    // expects LBAFormat0, LBAFormat1 etc
    if (!ty.starts_with("LBAFormat"))
    {
        BMCWEB_LOG_DEBUG << "wrong start";
        return std::nullopt;
    }

    ty.remove_prefix(std::min(ty.size(), strlen("LBAFormat")));

    size_t v;
    auto e = std::from_chars(ty.data(), ty.data() + ty.size(), v);
    if (e.ptr != ty.data() + ty.size() || e.ec != std::errc())
    {
        return std::nullopt;
    }
    return v;
}

inline void storageVolumeCreateHandler(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName, const std::string& storageId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        BMCWEB_LOG_DEBUG << "Failed to setup Redfish Route for StorageVolume";
        return;
    }
    if (systemName != "system")
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        BMCWEB_LOG_DEBUG << "Failed to find ComputerSystem of " << systemName;
        return;
    }

    uint64_t size;
    std::string lbaFormat;
    // allow to default, non-metadata formats ignore the parameter
    std::optional<bool> metadataAtEnd = false;
    std::optional<std::string> name;

    if (!json_util::readJsonAction(
            req, asyncResp->res, "Name", name, "Capacity/Data/ProvisionedBytes",
            size, "NVMeNamespaceProperties/LBAFormat/LBAFormatType", lbaFormat,
            "NVMeNamespaceProperties/LBAFormat/MetadataTransferredAtEndOfDataLBA",
            metadataAtEnd))
    {
        BMCWEB_LOG_DEBUG << "create volume json input failed";
        return;
    }

    std::optional<size_t> lbaIndex = parseLbaFormatType(lbaFormat);
    if (!lbaIndex)
    {
        BMCWEB_LOG_DEBUG << "Bad parsing lbaFormatType";
        messages::propertyValueNotInList(
            asyncResp->res, lbaFormat,
            "NVMeNamespaceProperties.LBAFormat.LBAFormatType");
        return;
    }

    findStorage(asyncResp, storageId,
                [req, asyncResp, size, lbaIndex, metadataAtEnd](
                    const sdbusplus::message::object_path& storagePath,
                    const std::string& storageService) {
        createStorageVolume(req, asyncResp, storagePath, storageService, size,
                            *lbaIndex, *metadataAtEnd);
    });
}

inline void storageVolumeDeleteHandler(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName, const std::string& storageId,
    const std::string& volumeId)
{
    BMCWEB_LOG_DEBUG << "delete handler vol " << volumeId;
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        BMCWEB_LOG_DEBUG << "Failed to setup Redfish Route for StorageVolume";
        return;
    }
    if (systemName != "system")
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        BMCWEB_LOG_DEBUG << "Failed to find ComputerSystem of " << systemName;
        return;
    }
    findStorageVolume(
        asyncResp, storageId, volumeId,
        [asyncResp, storageId,
         volumeId](const std::string& path, const std::string& connectionName,
                   const dbus::utility::MapperServiceMap& ifaces) {
        (void)ifaces;
        deleteStorageVolume(asyncResp, storageId, connectionName, path);
        });
}

inline void storageVolumeCollectionHandler(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName, const std::string& storageId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        BMCWEB_LOG_DEBUG
            << "Failed to setup Redfish Route for StorageVolume Collection";
        return;
    }
    if (systemName != "system")
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        BMCWEB_LOG_DEBUG << "Failed to find ComputerSystem of " << systemName;
        return;
    }

    findStorage(asyncResp, storageId,
                [asyncResp, storageId](
                    const sdbusplus::message::object_path& storagePath) {
        asyncResp->res.jsonValue["@odata.type"] =
            "#VolumeCollection.VolumeCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            crow::utility::urlFromPieces("redfish", "v1", "Systems", "system",
                                         "Storage", storageId, "Volumes");
        asyncResp->res.jsonValue["Name"] = "Storage Volume Collection";

        storageVolumes(storagePath,
                       [asyncResp, storageId](
                           const boost::system::error_code& ec,
                           const dbus::utility::MapperGetSubTreePathsResponse&
                               volumeList) {
            populateStorageVolumeCollection(asyncResp, ec, storageId,
                                            volumeList);
        });
    });
}

inline std::string lookupRelativePerformance(const std::string& rp)
{
    if (rp == "xyz.openbmc_project.Nvme.Storage.RelativePerformance.Best")
    {
        return "Best";
    }
    else if (rp ==
             "xyz.openbmc_project.Nvme.Storage.RelativePerformance.Better")
    {
        return "Better";
    }
    else if (rp == "xyz.openbmc_project.Nvme.Storage.RelativePerformance.Good")
    {
        return "Good";
    }
    return "Degraded";
}

inline void storageVolumeCapabilitiesHandler(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName, const std::string& storageId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        BMCWEB_LOG_DEBUG
            << "Failed to setup Redfish Route for StorageVolume Capabilities";
        return;
    }
    if (systemName != "system")
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        BMCWEB_LOG_DEBUG << "Failed to find ComputerSystem of " << systemName;
        return;
    }

    findStorage(asyncResp, storageId,
                [asyncResp,
                 storageId](const sdbusplus::message::object_path& storagePath,
                            const std::string service) {
        sdbusplus::asio::getProperty<
            std::vector<std::tuple<size_t, size_t, size_t, std::string>>>(
            *crow::connections::systemBus, service, storagePath,
            "xyz.openbmc_project.Nvme.Storage", "SupportedFormats",
            [asyncResp, storageId](
                const boost::system::error_code& ec,
                const std::vector<
                    std::tuple<size_t, size_t, size_t, std::string>>& formats) {
            if (ec)
            {
                messages::internalError(asyncResp->res);
                return;
            }
            asyncResp->res.jsonValue["@odata.type"] = "#Volume.v1_9_0.Volume";
            auto url = crow::utility::urlFromPieces(
                "redfish", "v1", "Systems", "system", "Storage", storageId,
                "Volumes", "Capabilities");
            asyncResp->res.jsonValue["@odata.id"] = url;
            asyncResp->res.jsonValue["Id"] = "Capabilities";
            asyncResp->res.jsonValue["Name"] = "Capabilities for Volumes";
            auto& nv = asyncResp->res.jsonValue["NVMeNamespaceProperties"];
            auto& allowable =
                nv["LBAFormatsSupported@Redfish.AllowableValues"] =
                    nlohmann::json::array_t();
            auto& formatDesc = nv["LBAFormats"] = nlohmann::json::array_t();

            for (auto& [index, blockSize, metadataSize, relPerf] : formats)
            {
                auto name = std::string("LBAFormat") + std::to_string(index);
                allowable.emplace_back(name);
                auto& f = formatDesc.emplace_back(nlohmann::json::object_t());
                auto rp = lookupRelativePerformance(relPerf);
                f["LBAFormatType"] = name;
                f["RelativePerformance"] = rp;
                f["LBADataSizeBytes"] = blockSize;
                f["LBAMetadataSizeBytes"] = metadataSize;
            }
            });
    });
}
inline void storageControllerHandler(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName, const std::string& storageId,
    const std::string& controllerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        BMCWEB_LOG_DEBUG
            << "Failed to setup Redfish Route for StorageController";
        return;
    }
    if (systemName != "system")
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        BMCWEB_LOG_DEBUG << "Failed to find ComputerSystem of " << systemName;
        return;
    }
    findStorage(asyncResp, storageId,
                [asyncResp, storageId, controllerId](
                    const sdbusplus::message::object_path& storagePath) {
        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.Inventory.Item.StorageController"};
        dbus::utility::getAssociatedSubTree(
            storagePath / "storage_controller",
            sdbusplus::message::object_path("/xyz/openbmc_project/inventory"),
            0, interfaces,
            [asyncResp, storageId, controllerId](
                const boost::system::error_code& ec,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            getStorageControllerHandler(asyncResp, storageId, controllerId, ec,
                                        subtree);
            });
    });
}

inline void requestRoutesStorageControllerCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Storage/<str>/Controllers/")
        .privileges(redfish::privileges::getStorageControllerCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(storageControllerCollectionHandler, std::ref(app)));
}

inline void requestRoutesStorageController(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/Storage/<str>/Controllers/<str>")
        .privileges(redfish::privileges::getStorageController)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(storageControllerHandler, std::ref(app)));
}

inline void requestRoutesStorageVolumeCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Storage/<str>/Volumes/")
        .privileges(redfish::privileges::getStorageVolumeCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(storageVolumeCollectionHandler, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Storage/<str>/Volumes/")
        .privileges(redfish::privileges::postStorageVolumeCollection)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(storageVolumeCreateHandler, std::ref(app)));

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/Storage/<str>/Volumes/Capabilities")
        .privileges(redfish::privileges::getStorageVolumeCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(storageVolumeCapabilitiesHandler, std::ref(app)));
}

inline void requestRoutesStorageVolume(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Storage/<str>/Volumes/<str>")
        .privileges(redfish::privileges::getStorageVolume)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(storageVolumeHandler, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Storage/<str>/Volumes/<str>")
        .privileges(redfish::privileges::deleteStorageVolume)
        .methods(boost::beast::http::verb::delete_)(
            std::bind_front(storageVolumeDeleteHandler, std::ref(app)));
}

} // namespace redfish
