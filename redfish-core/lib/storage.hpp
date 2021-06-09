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

#include "health.hpp"
#include "openbmc_dbus_rest.hpp"

#include <app.hpp>
#include <dbus_utility.hpp>
#include <query.hpp>
#include <registries/privilege_registry.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/unpack_properties.hpp>
#include <utils/dbus_utils.hpp>
#include <utils/location_utils.hpp>
#include <utils/log_utils.hpp>

#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace redfish
{
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
        collection_util::getCollectionMembers(
            asyncResp,
            crow::utility::urlFromPieces("redfish", "v1", "Systems", "system",
                                         "Storage"),
            {"xyz.openbmc_project.Inventory.Item.Storage"});
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
        collection_util::getCollectionMembers(
            asyncResp, crow::utility::urlFromPieces("redfish", "v1", "Storage"),
            {"xyz.openbmc_project.Inventory.Item.Storage"});
        });
}

inline void getDrives(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::shared_ptr<HealthPopulate>& health,
                      const sdbusplus::message::object_path& storagePath,
                      const std::string& chassisId)
{

    crow::connections::systemBus->async_method_call(
        [asyncResp, health, storagePath, chassisId](
            const boost::system::error_code ec,
            const dbus::utility::MapperGetSubTreePathsResponse& driveList) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "Drive mapper call error";
            messages::internalError(asyncResp->res);
            return;
        }
        sdbusplus::asio::getProperty<std::vector<std::string>>(
            *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper",
            (storagePath / "drive").str, "xyz.openbmc_project.Association",
            "endpoints",
            [asyncResp, health, storagePath, chassisId,
             driveList](const boost::system::error_code ec2,
                        const std::vector<std::string>& driveAssociations) {
            if (ec2)
            {
                BMCWEB_LOG_DEBUG << storagePath.str
                                 << " has no Drive association";
                return;
            }

            std::unordered_set<std::string> driveMap(driveList.begin(),
                                                     driveList.end());

            nlohmann::json& driveArray = asyncResp->res.jsonValue["Drives"];
            driveArray = nlohmann::json::array();
            auto& count = asyncResp->res.jsonValue["Drives@odata.count"];
            count = 0;

            for (const std::string& drivePath : driveAssociations)
            {
                sdbusplus::message::object_path path(drivePath);
                const std::string leaf = path.filename();
                if (leaf.empty())
                {
                    BMCWEB_LOG_DEBUG << "filename() is empty for " << drivePath;
                    continue;
                }

                if (driveMap.find(drivePath) == driveMap.end())
                {
                    BMCWEB_LOG_DEBUG
                        << "Associated Drive is does not have valid Drive interface: "
                        << drivePath;
                    continue;
                }

                health->inventory.emplace_back(drivePath);
                nlohmann::json::object_t drive;
                drive["@odata.id"] = crow::utility::urlFromPieces(
                    "redfish", "v1", "Chassis", chassisId, "Drives", leaf);
                driveArray.push_back(std::move(drive));
            }
            count = asyncResp->res.jsonValue["Drives"].size();
            });
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
        "/xyz/openbmc_project/inventory", int32_t(0),
        std::array<const char*, 1>{"xyz.openbmc_project.Inventory.Item.Drive"});
}

inline void
    getStorageLocation(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::string& path, const std::string& service,
                       const std::vector<std::string>& interfaces)
{
    for (const auto& interface : interfaces)
    {
        if (interface == "xyz.openbmc_project.Inventory.Decorator.LocationCode")
        {
            location_util::getLocationCode(asyncResp, service, path,
                                           "/PhysicalLocation"_json_pointer);
        }
        if (location_util::isConnector(interface))
        {
            std::optional<std::string> locationType =
                location_util::getLocationType(interface);
            if (!locationType)
            {
                BMCWEB_LOG_DEBUG << "getLocationType for Storage failed for "
                                 << interface;
                continue;
            }
            asyncResp->res
                .jsonValue["PhysicalLocation"]["PartLocation"]["LocationType"] =
                *locationType;
        }
    }
}

inline void getStorageControllerLocation(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& path, const std::string& service,
    const std::vector<std::string>& interfaces, size_t index)
{
    nlohmann::json::json_pointer locationPtr =
        "/StorageControllers"_json_pointer / index / "Location";
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

inline void populateStorageControllers(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& connectionName, const std::string& path,
    const std::vector<std::string>& interfaces, const std::string& id,
    const std::string& storageId)
{
    nlohmann::json& root = asyncResp->res.jsonValue["StorageControllers"];
    size_t index = root.size();
    nlohmann::json& storageController =
        root.emplace_back(nlohmann::json::object());

    storageController["@odata.type"] = "#Storage.v1_9_1.StorageController";
    storageController["@odata.id"] =
        crow::utility::urlFromPieces("redfish", "v1", "Systems", "system",
                                     "Storage", storageId)
            .set_fragment(crow::utility::urlFromPieces("StorageControllers",
                                                       std::to_string(index))
                              .string());
    storageController["Name"] = id;
    storageController["MemberId"] = id;
    storageController["Status"]["State"] = "Enabled";
    getStorageControllerLocation(asyncResp, path, connectionName, interfaces,
                                 index);

    sdbusplus::asio::getProperty<bool>(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Item", "Present",
        [asyncResp, index](const boost::system::error_code ec2, bool enabled) {
        // this interface isn't necessary, only check it
        // if we get a good return
        if (ec2)
        {
            return;
        }
        if (!enabled)
        {
            asyncResp->res
                .jsonValue["StorageControllers"][index]["Status"]["State"] =
                "Disabled";
        }
        });

    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Decorator.Asset",
        [asyncResp, index](
            const boost::system::error_code ec2,
            const std::vector<std::pair<
                std::string, dbus::utility::DbusVariantType>>& propertiesList) {
        if (ec2)
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

        nlohmann::json& controller =
            asyncResp->res.jsonValue["StorageControllers"][index];

        if (partNumber != nullptr)
        {
            controller["PartNumber"] = *partNumber;
        }

        if (serialNumber != nullptr)
        {
            controller["SerialNumber"] = *serialNumber;
        }

        if (manufacturer != nullptr)
        {
            controller["Manufacturer"] = *manufacturer;
        }

        if (model != nullptr)
        {
            controller["Model"] = *model;
        }
        });
}

inline void getStorageControllersWithAssociation(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::shared_ptr<HealthPopulate>& health,
    const dbus::utility::MapperGetSubTreeResponse& subtree,
    const std::string& storagePath, const std::string& storageId)
{

    std::unordered_map<std::string,
                       std::pair<std::string, std::vector<std::string>>>
        storageControllerServices;
    for (const auto& [path, interfaceDict] : subtree)
    {
        if (interfaceDict.size() != 1)
        {
            BMCWEB_LOG_ERROR << "Connection size " << interfaceDict.size()
                             << ", not equal to 1";
            continue;
        }
        storageControllerServices.emplace(path, interfaceDict.front());
    }

    sdbusplus::asio::getProperty<std::vector<std::string>>(
        *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper",
        storagePath + "/storage_controller", "xyz.openbmc_project.Association",
        "endpoints",
        [asyncResp, storageId, storageControllerServices, storagePath,
         health](const boost::system::error_code ec,
                 const std::vector<std::string>& storageControllerList) {
        if (ec)
        {
            BMCWEB_LOG_ERROR
                << "Failed to find Storage Controller association for "
                << storagePath;
            return;
        }

        nlohmann::json& root = asyncResp->res.jsonValue["StorageControllers"];
        root = nlohmann::json::array();
        std::vector<std::string> storageControllerPaths;
        for (const std::string& storageController : storageControllerList)
        {
            auto storageControllerService =
                storageControllerServices.find(storageController);
            if (storageControllerService == storageControllerServices.end())
            {
                continue;
            }

            sdbusplus::message::object_path object(
                storageControllerService->first);
            std::string id = object.filename();
            if (id.empty())
            {
                BMCWEB_LOG_ERROR << "Failed to find filename in "
                                 << storageControllerService->first;
                return;
            }

            const std::string& connectionName =
                storageControllerService->second.first;
            const std::vector<std::string>& interfaces =
                storageControllerService->second.second;
            populateStorageControllers(asyncResp, connectionName,
                                       storageControllerService->first,
                                       interfaces, id, storageId);
            storageControllerPaths.emplace_back(
                storageControllerService->first);
        }

        // this is done after we know the json array will no longer
        // be resized, as json::array uses vector underneath and we
        // need references to its members that won't change
        size_t count = 0;
        // Pointer based on |asyncResp->res.jsonValue|
        nlohmann::json::json_pointer rootPtr =
            "/StorageControllers"_json_pointer;
        for (const std::string& path : storageControllerPaths)
        {
            auto subHealth = std::make_shared<HealthPopulate>(
                asyncResp, rootPtr / count / "Status");
            subHealth->inventory.emplace_back(path);
            health->inventory.emplace_back(path);
            health->children.emplace_back(subHealth);
            log_utils::getChassisLogEntry(
                asyncResp,
                "/StorageControllers"_json_pointer / count / "Status", path,
                "OpenBMC.0.2.0.StorageControllerError");
            count++;
        }
        });
}

inline void
    getStorageControllers(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::shared_ptr<HealthPopulate>& health,
                          const std::string& storagePath,
                          const std::string& storageId)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp, health, storagePath,
         storageId](const boost::system::error_code ec,
                    const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec || subtree.empty())
        {
            // doesn't have to be there
            return;
        }

        getStorageControllersWithAssociation(asyncResp, health, subtree,
                                             storagePath, storageId);
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory", int32_t(0),
        std::array<const char*, 1>{
            "xyz.openbmc_project.Inventory.Item.StorageController"});
}

inline void
    getDriveFromChassis(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        const std::shared_ptr<HealthPopulate>& health,
                        const sdbusplus::message::object_path& storagePath)
{

    crow::connections::systemBus->async_method_call(
        [asyncResp, health, storagePath](
            const boost::system::error_code ec,
            const dbus::utility::MapperGetSubTreePathsResponse& chassisList) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "Chassis mapper call error";
            messages::internalError(asyncResp->res);
            return;
        }
        sdbusplus::asio::getProperty<std::vector<std::string>>(
            *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper",
            (storagePath / "chassis").str, "xyz.openbmc_project.Association",
            "endpoints",
            [asyncResp, health, storagePath,
             chassisList](const boost::system::error_code ec2,
                          const std::vector<std::string>& chassisAssociations) {
            if (ec2)
            {
                BMCWEB_LOG_DEBUG << storagePath.str
                                 << " has no Chassis association";
                return;
            }

            if (chassisAssociations.size() != 1)
            {
                BMCWEB_LOG_WARNING << "Storage is associated to not 1 Chassis";
            }

            std::unordered_set<std::string> chassisMap(chassisList.begin(),
                                                       chassisList.end());
            const std::string& chassisPath = chassisAssociations.front();
            if (chassisMap.find(chassisPath) == chassisMap.end())
            {
                BMCWEB_LOG_WARNING << "Failed to find Chassis with "
                                   << chassisPath;
                return;
            }

            std::string chassisId =
                sdbusplus::message::object_path(chassisPath).filename();
            if (chassisId.empty())
            {
                BMCWEB_LOG_ERROR << "Failed to find filename in "
                                 << chassisPath;
                return;
            }

            getDrives(asyncResp, health, storagePath, chassisId);
            });
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
        "/xyz/openbmc_project/inventory", int32_t(0),
        std::array<const char*, 2>{
            "xyz.openbmc_project.Inventory.Item.Board",
            "xyz.openbmc_project.Inventory.Item.Chassis"});
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

        crow::connections::systemBus->async_method_call(
            [asyncResp, storageId](
                const boost::system::error_code ec,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "requestRoutesStorage DBUS response error";
                messages::resourceNotFound(
                    asyncResp->res, "#Storage.v1_9_1.Storage", storageId);
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
                    asyncResp->res, "#Storage.v1_9_1.Storage", storageId);
                return;
            }

            const std::vector<std::pair<std::string, std::vector<std::string>>>&
                connectionNames = storage->second;
            if (connectionNames.size() != 1)
            {
                BMCWEB_LOG_ERROR << "Connection size " << connectionNames.size()
                                 << ", greater than 1";
                messages::resourceNotFound(
                    asyncResp->res, "#Storage.v1_9_1.Storage", storageId);
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] = "#Storage.v1_9_1.Storage";
            asyncResp->res.jsonValue["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Systems",
                                             "system", "Storage", storageId);
            asyncResp->res.jsonValue["Name"] = "Storage";
            asyncResp->res.jsonValue["Id"] = storageId;
            asyncResp->res.jsonValue["Status"]["State"] = "Enabled";

            auto health = std::make_shared<HealthPopulate>(asyncResp);
            health->populate();

            getDriveFromChassis(asyncResp, health, storage->first);
            getStorageControllers(asyncResp, health, storage->first, storageId);
            getStorageLocation(asyncResp, connectionNames[0].first,
                               storage->first, connectionNames[0].second);
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree",
            "/xyz/openbmc_project/inventory", 0,
            std::array<std::string, 1>{
                "xyz.openbmc_project.Inventory.Item.Storage"});
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

        crow::connections::systemBus->async_method_call(
            [asyncResp, storageId](
                const boost::system::error_code ec,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "requestRoutesStorage DBUS response error";
                messages::resourceNotFound(
                    asyncResp->res, "#Storage.v1_9_1.Storage", storageId);
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
                    asyncResp->res, "#Storage.v1_9_1.Storage", storageId);
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] = "#Storage.v1_9_1.Storage";
            asyncResp->res.jsonValue["@odata.id"] =
                crow::utility::urlFromPieces("redfish", "v1", "Storage",
                                             storageId);
            asyncResp->res.jsonValue["Name"] = "Storage";
            asyncResp->res.jsonValue["Id"] = storageId;
            asyncResp->res.jsonValue["Status"]["State"] = "Enabled";
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
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree",
            "/xyz/openbmc_project/inventory", 0,
            std::array<std::string, 1>{
                "xyz.openbmc_project.Inventory.Item.Storage"});
        });
}

inline void getDriveAsset(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& connectionName,
                          const std::string& path)
{
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, connectionName, path,
        "xyz.openbmc_project.Inventory.Decorator.Asset",
        [asyncResp](const boost::system::error_code ec,
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
        [asyncResp, path](const boost::system::error_code ec,
                          const bool enabled) {
        // this interface isn't necessary, only check it if
        // we get a good return
        if (ec)
        {
            return;
        }

        if (!enabled)
        {
            asyncResp->res.jsonValue["Status"]["State"] = "Disabled";
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
        [asyncResp](const boost::system::error_code ec, const bool updating) {
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
            const boost::system::error_code ec,
            const dbus::utility::DBusPropertiesMap& propertiesList) {
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
                if (value != nullptr && *value)
                {
                    addResetLinks(
                        asyncResp->res.jsonValue["Actions"]["#Drive.Reset"],
                        driveId, *chassisId);
                }
            }
        }
        });
}

static void
    addAllDriveInfo(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                    const std::string& driveId,
                    const std::string& connectionName, const std::string& path,
                    const std::vector<std::string>& interfaces,
                    const std::optional<std::string> chassisId = std::nullopt)
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

inline void requestRoutesDrive(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Storage/1/Drives/<str>/")
        .privileges(redfish::privileges::getDrive)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName, const std::string& driveId) {
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

        crow::connections::systemBus->async_method_call(
            [asyncResp,
             driveId](const boost::system::error_code ec,
                      const dbus::utility::MapperGetSubTreeResponse& subtree) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "Drive mapper call error";
                messages::internalError(asyncResp->res);
                return;
            }

            auto drive = std::find_if(
                subtree.begin(), subtree.end(),
                [&driveId](
                    const std::pair<std::string,
                                    dbus::utility::MapperServiceMap>& object) {
                return sdbusplus::message::object_path(object.first)
                           .filename() == driveId;
                });

            if (drive == subtree.end())
            {
                messages::resourceNotFound(asyncResp->res, "Drive", driveId);
                return;
            }

            const std::string& path = drive->first;
            const dbus::utility::MapperServiceMap& connectionNames =
                drive->second;

            asyncResp->res.jsonValue["@odata.type"] = "#Drive.v1_7_0.Drive";
            asyncResp->res.jsonValue["@odata.id"] =
                "/redfish/v1/Systems/system/Storage/1/Drives/" + driveId;
            asyncResp->res.jsonValue["Name"] = driveId;
            asyncResp->res.jsonValue["Id"] = driveId;

            if (connectionNames.size() != 1)
            {
                BMCWEB_LOG_ERROR << "Connection size " << connectionNames.size()
                                 << ", not equal to 1";
                messages::internalError(asyncResp->res);
                return;
            }

            getMainChassisId(
                asyncResp, [](const std::string& chassisId,
                              const std::shared_ptr<bmcweb::AsyncResp>& aRsp) {
                    aRsp->res.jsonValue["Links"]["Chassis"]["@odata.id"] =
                        "/redfish/v1/Chassis/" + chassisId;
                });

            // default it to Enabled
            asyncResp->res.jsonValue["Status"]["State"] = "Enabled";

            auto health = std::make_shared<HealthPopulate>(asyncResp);
            health->inventory.emplace_back(path);
            health->populate();

            addAllDriveInfo(asyncResp, driveId, connectionNames[0].first, path,
                            connectionNames[0].second);
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree",
            "/xyz/openbmc_project/inventory", int32_t(0),
            std::array<const char*, 1>{
                "xyz.openbmc_project.Inventory.Item.Drive"});
        });
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
    crow::connections::systemBus->async_method_call(
        [asyncResp,
         chassisId](const boost::system::error_code ec,
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
            sdbusplus::asio::getProperty<std::vector<std::string>>(
                *crow::connections::systemBus,
                "xyz.openbmc_project.ObjectMapper", path + "/drive",
                "xyz.openbmc_project.Association", "endpoints",
                [asyncResp, chassisId](const boost::system::error_code ec3,
                                       const std::vector<std::string>& resp) {
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
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory", 0,
        std::array<const char*, 2>{
            "xyz.openbmc_project.Inventory.Item.Board",
            "xyz.openbmc_project.Inventory.Item.Chassis"});
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
                       const boost::system::error_code ec,
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
        const std::array<const char*, 1> driveInterface = {
            "xyz.openbmc_project.Inventory.Item.Drive"};

        crow::connections::systemBus->async_method_call(
            [asyncResp, chassisId, driveName](
                const boost::system::error_code ec,
                const dbus::utility::MapperGetSubTreeResponse& subtree) {
            buildDrive(asyncResp, chassisId, driveName, ec, subtree);
            },
            "xyz.openbmc_project.ObjectMapper",
            "/xyz/openbmc_project/object_mapper",
            "xyz.openbmc_project.ObjectMapper", "GetSubTree",
            "/xyz/openbmc_project/inventory", 0, driveInterface);
    }
}

void findChassisDrive(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& chassisId, const std::string& driveId,
                      std::function<void(const boost::system::error_code ec3,
                                         const std::vector<std::string>& resp)>
                          cb)
{
    const std::array<const char*, 2> interfaces = {
        "xyz.openbmc_project.Inventory.Item.Board",
        "xyz.openbmc_project.Inventory.Item.Chassis"};

    // mapper call chassis
    crow::connections::systemBus->async_method_call(
        [asyncResp, chassisId, driveId,
         cb](const boost::system::error_code ec,
             const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (ec)
        {
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

            sdbusplus::asio::getProperty<std::vector<std::string>>(
                *crow::connections::systemBus,
                "xyz.openbmc_project.ObjectMapper", path + "/drive",
                "xyz.openbmc_project.Association", "endpoints", cb);
            break;
        }
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory", 0, interfaces);
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

    findChassisDrive(asyncResp, chassisId, driveName,
                     [asyncResp, chassisId,
                      driveName](const boost::system::error_code ec3,
                                 const std::vector<std::string>& resp) {
        if (ec3)
        {
            return; // no drives = no failures
        }
        matchAndFillDrive(asyncResp, chassisId, driveName, resp);
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
    const char* interfaceName = "xyz.openbmc_project.State.Drive";

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
    crow::connections::systemBus->async_method_call(
        [asyncResp, driveId, action, interfaceName](
            const boost::system::error_code mapperEc,
            const dbus::utility::MapperGetSubTreeResponse& subtree) {
        if (mapperEc)
        {
            BMCWEB_LOG_ERROR << "DBUS response error";
            messages::internalError(asyncResp->res);
            return;
        }

        auto driveState = std::find_if(subtree.begin(), subtree.end(),
                                       [&driveId](auto& object) {
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

        const std::string& connectionName = connectionNames[0].first;
        const char* destProperty = "RequestedDriveTransition";
        std::variant<std::string> dbusPropertyValue(action);

        crow::connections::systemBus->async_method_call(
            [asyncResp, action](const boost::system::error_code ec) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "[Set] Bad D-Bus request error for "
                                 << action << " : " << ec;
                messages::internalError(asyncResp->res);
                return;
            }

            messages::success(asyncResp->res);
            },
            connectionName, path, "org.freedesktop.DBus.Properties", "Set",
            interfaceName, destProperty, dbusPropertyValue);
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/inventory", int32_t(0),
        std::array<const char*, 1>{interfaceName});
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

        findChassisDrive(asyncResp, chassisId, driveId,
                         [asyncResp, chassisId, driveId,
                          resetType](const boost::system::error_code ec2,
                                     const std::vector<std::string>& drives) {
            if (ec2)
            {
                BMCWEB_LOG_ERROR << "failed to find drives";
                messages::internalError(asyncResp->res);
                return; // no drives = no failures
            }

            std::unordered_set<std::string> drivesMap(drives.begin(),
                                                      drives.end());
            std::array<const char*, 2> driveInterfaces = {
                "xyz.openbmc_project.Inventory.Item.Drive",
                "xyz.openbmc_project.State.Drive"};

            crow::connections::systemBus->async_method_call(
                [asyncResp, driveId, resetType,
                 drivesMap](const boost::system::error_code ec3,
                            const dbus::utility::MapperGetSubTreeResponse&
                                driveSubtree) {
                if (ec3)
                {
                    BMCWEB_LOG_ERROR << "Drive mapper call error ";
                    messages::internalError(asyncResp->res);
                    return;
                }

                auto drive = std::find_if(
                    driveSubtree.begin(), driveSubtree.end(),
                    [&driveId, &drivesMap](
                        const std::pair<std::string,
                                        dbus::utility::MapperServiceMap>&
                            object) {
                    return sdbusplus::message::object_path(object.first)
                                   .filename() == driveId &&
                           drivesMap.contains(object.first);
                    });

                if (drive == driveSubtree.end())
                {
                    messages::resourceNotFound(asyncResp->res,
                                               "Drive Action Reset", driveId);
                    return;
                }

                const std::string& drivePath = drive->first;
                const dbus::utility::MapperServiceMap& driveConnections =
                    drive->second;

                if (driveConnections.size() != 1)
                {
                    BMCWEB_LOG_ERROR << "Connection size "
                                     << driveConnections.size()
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
                    BMCWEB_LOG_ERROR
                        << "Drive does not have the required interfaces ";
                    messages::internalError(asyncResp->res);
                    return;
                }

                sdbusplus::asio::getProperty<bool>(
                    *crow::connections::systemBus, driveConnections[0].first,
                    drivePath, "xyz.openbmc_project.Inventory.Item.Drive",
                    "Resettable",
                    [asyncResp, driveId,
                     resetType](const boost::system::error_code propEc,
                                bool resettable) {
                    if (propEc)
                    {
                        BMCWEB_LOG_ERROR
                            << "Failed to get resettable property ";
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    if (!resettable)
                    {
                        messages::actionNotSupported(
                            asyncResp->res,
                            "The drive does not support resets.");
                        return;
                    }
                    performDriveReset(asyncResp, driveId, resetType);
                    });
                },
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetSubTree",
                "/xyz/openbmc_project/inventory", int32_t(0), driveInterfaces);
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
        findChassisDrive(asyncResp, chassisId, driveId,
                         [asyncResp, chassisId,
                          driveId](const boost::system::error_code ec2,
                                   const std::vector<std::string>& drives) {
            if (ec2)
            {
                BMCWEB_LOG_ERROR << "failed to find drives";
                messages::internalError(asyncResp->res);
                return; // no drives = no failures
            }

            std::unordered_set<std::string> drivesMap(drives.begin(),
                                                      drives.end());

            std::array<const char*, 2> driveInterfaces = {
                "xyz.openbmc_project.Inventory.Item.Drive",
                "xyz.openbmc_project.State.Drive"};

            crow::connections::systemBus->async_method_call(
                [asyncResp, chassisId, driveId,
                 drivesMap](const boost::system::error_code ec3,
                            const dbus::utility::MapperGetSubTreeResponse&
                                driveSubtree) {
                if (ec3)
                {
                    BMCWEB_LOG_ERROR << "Drive mapper call error";
                    messages::internalError(asyncResp->res);
                    return;
                }

                auto drive = std::find_if(
                    driveSubtree.begin(), driveSubtree.end(),
                    [&driveId, &drivesMap](
                        const std::pair<
                            std::string,
                            std::vector<std::pair<std::string,
                                                  std::vector<std::string>>>>&
                            object) {
                    return sdbusplus::message::object_path(object.first)
                                   .filename() == driveId &&
                           drivesMap.contains(object.first);
                    });

                if (drive == driveSubtree.end())
                {
                    messages::resourceNotFound(
                        asyncResp->res, "Drive ResetActionInfo", driveId);
                    return;
                }

                const std::string& drivePath = drive->first;
                const dbus::utility::MapperServiceMap& driveConnections =
                    drive->second;

                if (driveConnections.size() != 1)
                {
                    BMCWEB_LOG_ERROR << "Connection size "
                                     << driveConnections.size()
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
                    BMCWEB_LOG_ERROR
                        << "Drive does not have the required interfaces ";
                    messages::internalError(asyncResp->res);
                    return;
                }

                sdbusplus::asio::getProperty<bool>(
                    *crow::connections::systemBus, driveConnections[0].first,
                    drivePath, "xyz.openbmc_project.Inventory.Item.Drive",
                    "Resettable",
                    [asyncResp, chassisId,
                     driveId](const boost::system::error_code propEc,
                              bool resettable) {
                    if (propEc)
                    {
                        BMCWEB_LOG_ERROR
                            << "Failed to get resettable property ";
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    if (!resettable)
                    {
                        messages::actionNotSupported(
                            asyncResp->res,
                            "The drive does not support resets.");
                        return;
                    }
                    asyncResp->res.jsonValue["@odata.type"] =
                        "#ActionInfo.v1_1_2.ActionInfo";
                    asyncResp->res.jsonValue["@odata.id"] =
                        crow::utility::urlFromPieces(
                            "redfish", "v1", "Chassis", chassisId, "Drives",
                            driveId, "ResetActionInfo");
                    asyncResp->res.jsonValue["Name"] = "Reset Action Info";
                    asyncResp->res.jsonValue["Id"] = "ResetActionInfo";
                    nlohmann::json::object_t parameters;
                    parameters["Name"] = "ResetType";
                    parameters["Required"] = true;
                    parameters["DataType"] = "String";
                    nlohmann::json::array_t allowableValues;
                    allowableValues.emplace_back("PowerCycle");
                    allowableValues.emplace_back("ForceRestart");
                    parameters["AllowableValues"] = std::move(allowableValues);
                    asyncResp->res.jsonValue["Parameters"] =
                        std::move(parameters);
                    });
                },
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetSubTree",
                "/xyz/openbmc_project/inventory", int32_t(0), driveInterfaces);
        });
        });
}

} // namespace redfish
