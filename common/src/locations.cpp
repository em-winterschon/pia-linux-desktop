// Copyright (c) 2022 Private Internet Access, Inc.
//
// This file is part of the Private Internet Access Desktop Client.
//
// The Private Internet Access Desktop Client is free software: you can
// redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version.
//
// The Private Internet Access Desktop Client is distributed in the hope that
// it will be useful, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with the Private Internet Access Desktop Client.  If not, see
// <https://www.gnu.org/licenses/>.

#include "common.h"
#line SOURCE_FILE("locations.cpp")

#include "locations.h"
#include <QJsonDocument>

namespace
{
    // Constants used for the manual region if one is created
    const QString manualRegionId{QStringLiteral("manual")};
    // Although service groups can be changed at any time by Ops (these are just
    // intended to abbreviate the regions list), defaults are present for a
    // manual region, as this is a dev tool only.  If the groups change, the
    // sevice groups can be specified in the override.
    const std::vector<QString> manualRegionDefaultGroups{
        QStringLiteral("ovpntcp"),
        QStringLiteral("ovpnudp"),
        QStringLiteral("wg")
    };

    // A few flags were added within a version of the regions list.  While these
    // should practically always be present now, we tolerate missing optional
    // flags and assume it was 'false'.
    bool getOptionalFlag(const QString &flagName, const QJsonObject &region, const QString &regionIdTrace)
    {
        if(region.contains(flagName))
        {
            try
            {
                return json_cast<bool>(region.value(flagName), HERE);
            }
            catch(const Error &ex)
            {
                qWarning() << QStringLiteral("Unable to read %1 flag of region").arg(flagName)
                    << regionIdTrace << "-" << ex;
            }
        }

        return false;
    }
}

const std::unordered_map<QString, QString> shadowsocksLegacyRegionMap
{
    {QStringLiteral("us_south_west"), QStringLiteral("us_dal")},
    {QStringLiteral("us_seattle"), QStringLiteral("us_sea")},
    {QStringLiteral("us-newjersey"), QStringLiteral("us_nyc")}, // "US East"
    {QStringLiteral("japan"), QStringLiteral("jp")},
    {QStringLiteral("uk-london"), QStringLiteral("uk")},
    {QStringLiteral("nl_amsterdam"), QStringLiteral("nl")}
};

// If the location ID given is present in the Shadowsocks region data, add a
// Shadowsocks server to the list of servers for this location.
//
// If the location ID doesn't have Shadowsocks, nothing happens.  If the
// Shadowsocks server data are invalid, the error is traced and no changes are
// made.
void addShadowsocksServer(std::vector<Server> &servers,
                          const QString &locationId,
                          const std::unordered_map<QString, QJsonObject> &shadowsocksServers)
{
    // Look for Shadowsocks info for this region
    auto itShadowsocksServer = shadowsocksServers.find(locationId);

    // The Shadowsocks region list still uses legacy location IDs, map some of
    // the next-gen location IDs to legacy IDs where they differ
    if(itShadowsocksServer == shadowsocksServers.end())
    {
        auto itMappedId = shadowsocksLegacyRegionMap.find(locationId);
        if(itMappedId != shadowsocksLegacyRegionMap.end())
            itShadowsocksServer = shadowsocksServers.find(itMappedId->second);
    }

    if(itShadowsocksServer != shadowsocksServers.end())
    {
        try
        {
            const auto &shadowsocksServer = itShadowsocksServer->second;
            Server shadowsocks;
            shadowsocks.ip(json_cast<QString>(shadowsocksServer.value(QStringLiteral("host")), HERE));
            // No serial, not provided (or used) for Shadowsocks
            shadowsocks.shadowsocksKey(json_cast<QString>(shadowsocksServer.value(QStringLiteral("key")), HERE));
            shadowsocks.shadowsocksCipher(json_cast<QString>(shadowsocksServer.value(QStringLiteral("cipher")), HERE));
            shadowsocks.shadowsocksPorts({json_cast<quint16>(shadowsocksServer.value(QStringLiteral("port")), HERE)});
            servers.push_back(std::move(shadowsocks));
        }
        catch(const Error &ex)
        {
            qWarning() << "Ignoring invalid Shadowsocks info for region"
                << locationId << "-" << ex;
        }
    }
}

void applyModernService(const QJsonObject &serviceObj, Server &groupTemplate,
                        const QString &groupTrace)
{
    try
    {
        const auto &serviceName = json_cast<QString>(serviceObj["name"], HERE);
        Service knownService;
        if(serviceName == QStringLiteral("openvpn_tcp"))
            knownService = Service::OpenVpnTcp;
        else if(serviceName == QStringLiteral("openvpn_udp"))
            knownService = Service::OpenVpnUdp;
        else if(serviceName == QStringLiteral("wireguard"))
            knownService = Service::WireGuard;
        else if(serviceName == QStringLiteral("meta"))
            knownService = Service::Meta;
        else
        {
            // Otherwise, some other service not used by Desktop - ignore silently
            // Shadowsocks does not appear in the main region list, it's in a
            // separate list and Desktop integrates the data into its model.
            return;
        }
        // Don't load "ports" until we know it's a service we use, some future
        // services might not have ports in the same way.
        auto servicePorts = json_cast<std::vector<quint16>>(serviceObj["ports"], HERE);
        groupTemplate.servicePorts(knownService, servicePorts);
    }
    catch(const Error &ex)
    {
        qWarning() << "Service in group" << groupTrace << "is not valid:" << ex
            << QJsonDocument{serviceObj}.toJson();
    }
}

void readModernServer(const Server &groupTemplate, std::vector<Server> &servers,
                      const QJsonObject &serverObj, const QString &rgnIdTrace)
{
    try
    {
        Server newServer{groupTemplate};
        newServer.ip(json_cast<QString>(serverObj["ip"], HERE));
        newServer.commonName(json_cast<QString>(serverObj["cn"], HERE));
        // The OpenVPN cipher negotation type (NCP or pia-signal-settings) is
        // indicated by the "van" property (short for "vanilla" - i.e. the
        // server has "vanilla OpenVPN" without the pia-signal-settings patch).
        //
        // This defaults to 'true' so that in the long term when the whole fleet
        // is on vanilla, we won't have any servers list bloat from this
        // property.  (If the property doesn't exist, serverObject["van"]
        // returns an undefined QJsonValue.)
        newServer.openvpnNcpSupport(serverObj["van"].toBool(true));
        servers.push_back(newServer);
    }
    catch(const Error &ex)
    {
        qWarning() << "Can't load server in location" << rgnIdTrace
            << "due to error:" << ex;
    }
}

QSharedPointer<Location> readModernLocation(const QJsonObject &regionObj,
                                            const std::unordered_map<QString, Server> &groupTemplates,
                                            const std::unordered_map<QString, QJsonObject> &shadowsocksServers)
{
    QSharedPointer<Location> pLocation{new Location{}};
    QString id; // For tracing, if we get an ID and the read fails, this will be traced
    try
    {
        pLocation->id(json_cast<QString>(regionObj["id"], HERE));
        id = pLocation->id();   // Found an id, trace it if the location fails
        pLocation->name(json_cast<QString>(regionObj["name"], HERE));
        pLocation->country(json_cast<QString>(regionObj["country"], HERE));
        pLocation->geoOnly(getOptionalFlag(QStringLiteral("geo"), regionObj, pLocation->id()));
        pLocation->autoSafe(json_cast<bool>(regionObj["auto_region"], HERE));
        pLocation->portForward(json_cast<bool>(regionObj["port_forward"], HERE));
        pLocation->autoSafe(json_cast<bool>(regionObj["auto_region"], HERE));
        pLocation->portForward(json_cast<bool>(regionObj["port_forward"], HERE));
        pLocation->offline(getOptionalFlag("offline", regionObj, pLocation->id()));
        // Build servers
        std::vector<Server> servers;
        const auto &serverGroupsObj = regionObj["servers"].toObject();
        auto itGroup = serverGroupsObj.begin();
        while(itGroup != serverGroupsObj.end())
        {
            // Find the group template
            auto itTemplate = groupTemplates.find(itGroup.key());
            if(itTemplate == groupTemplates.end())
            {
                qWarning() << "Group" << itGroup.key() << "not known in location"
                    << pLocation->id();
                // Skip all servers in this group
            }
            // If the group template has no known services, skip this group
            // silently.  This can be normal for services not used by Desktop.
            else if(itTemplate->second.hasNonLatencyService())
            {
                for(const auto &serverValue : itGroup->toArray())
                {
                    const auto &serverObj = serverValue.toObject();
                    readModernServer(itTemplate->second, servers, serverObj,
                        pLocation->id());
                }
            }
            ++itGroup;
        }

        // Include the Shadowsocks server for this location if present
        addShadowsocksServer(servers, pLocation->id(), shadowsocksServers);

        pLocation->servers(servers);
    }
    catch(const Error &ex)
    {
        qWarning() << "Can't load location" << id << "due to error" << ex;
        return {};
    }

    // If the location was loaded but has no servers, treat that as if the location
    // is offline
    if(pLocation && pLocation->servers().empty())
    {
        pLocation->offline(true);
        qWarning() << "Location" << pLocation->id() << "has no servers, setting as offline";
    }

    return pLocation;
}

// Build a set of Servers from a specific IP/CN and a set of service groups that
// apply to them.  This is used for Dedicated IP regions and the manual region.
// The region ID is just used for tracing.
std::vector<Server> buildServersFromGroups(const std::vector<QString> &groups,
                                           const QString &ip, const QString &cn,
                                           bool openvpnNcpSupport,
                                           const QString &regionId,
                                           const std::unordered_map<QString, Server> &groupTemplates)
{
    std::vector<Server> servers;
    for(const auto &group : groups)
    {
        // Find the group template
        auto itTemplate = groupTemplates.find(group);
        if(itTemplate == groupTemplates.end())
        {
            qWarning() << "Group" << group << "not known in location"
                << regionId;
            // Skip this group
        }
        // Ignore groups that have no services used by Desktop, this can be
        // normal if it had other services that we don't care about
        else if(itTemplate->second.hasNonLatencyService())
        {
            // Create a server for this group using the DIP IP/CN
            Server newServer{itTemplate->second};
            newServer.ip(ip);
            newServer.commonName(cn);
            newServer.openvpnNcpSupport(openvpnNcpSupport);
            servers.push_back(std::move(newServer));
        }
    }
    return servers;
}

// Copy the 'meta' servers from the given region to the servers vector.  Only
// 'meta' services are copied, even if a server has both 'meta' and some other
// service.
void copyRegionMetaServers(const Location &correspondingLocation,
                           std::vector<Server> &servers)
{
    // Use the 'meta' service from server(s) in the corresponding location,
    // but not any other services
    for(const auto &correspondingServer : correspondingLocation.servers())
    {
        if(correspondingServer.hasService(Service::Meta))
        {
            // Create a new server and copy over just the 'meta' info; do
            // not take any other services that might be offered on this
            // server.
            Server metaServer{};
            metaServer.ip(correspondingServer.ip());
            metaServer.commonName(correspondingServer.commonName());
            metaServer.metaPorts(correspondingServer.metaPorts());
            servers.push_back(std::move(metaServer));
        }
    }
}

// Build a Location from a dedicated IP.  Location metadata (name, country,
// geo, PF, etc.) are taken from the corresponding normal location.
QSharedPointer<Location> buildDedicatedIpLocation(const LocationsById &modernLocations,
                                                  const std::unordered_map<QString, Server> &groupTemplates,
                                                  const AccountDedicatedIp &dip)
{
    QSharedPointer<Location> pLocation{new Location{}};

    // Set up the essential parts (ID and servers) of the location that we know
    // even if the corresponding location is not found for some reason.
    pLocation->id(dip.id());
    // DIP locations are never selected automatically
    pLocation->autoSafe(false);
    pLocation->dedicatedIp(dip.ip());
    pLocation->dedicatedIpExpire(dip.expire());
    pLocation->dedicatedIpCorrespondingRegion(dip.regionId());
    // Dedicated IP servers are still using 'pia-signal-settings' cipher
    // negotiation, we'll add an indication for this in a later API revision so
    // these can eventually migrate
    std::vector<Server> servers{buildServersFromGroups(dip.serviceGroups(),
                                                       dip.ip(), dip.cn(),
                                                       false,
                                                       pLocation->id(),
                                                       groupTemplates)};

    // Try to find the corresponding location for metadata
    auto itCorrespondingLocation = modernLocations.find(dip.regionId());
    if(itCorrespondingLocation == modernLocations.end())
    {
        // Couldn't find the location - set defaults.  The DIP region will still
        // be available, but without country/name info
        pLocation->name({});
        pLocation->country({});
        pLocation->portForward(false);
        pLocation->geoOnly(false);
    }
    else
    {
        pLocation->name(itCorrespondingLocation->second->name());
        pLocation->country(itCorrespondingLocation->second->country());
        pLocation->portForward(itCorrespondingLocation->second->portForward());
        pLocation->geoOnly(itCorrespondingLocation->second->geoOnly());
        copyRegionMetaServers(*itCorrespondingLocation->second, servers);
    }

    pLocation->servers(std::move(servers));
    return pLocation;
}

// If a manual location has been specified, build it.
QSharedPointer<Location> buildManualLocation(const LocationsById &modernLocations,
                                             const std::unordered_map<QString, Server> &groupTemplates,
                                             const ManualServer &manualServer)
{
    // If all parts of the manual location are empty, ignore it without tracing,
    // this is normal.
    if(manualServer.ip().isEmpty() && manualServer.cn().isEmpty() &&
        manualServer.serviceGroups().empty() &&
        manualServer.correspondingRegionId().isEmpty())
    {
        return {};
    }

    // If, somehow, there's already a region with this ID, keep that one
    if(modernLocations.count(manualRegionId) > 0)
    {
        qWarning() << "Can't build manual region, region with ID"
            << manualRegionId << "already exists";
        return {};
    }

    // If the IP and CN aren't both set, the manual server isn't correctly
    // configured
    if(manualServer.ip().isEmpty() || manualServer.cn().isEmpty())
    {
        qWarning() << "Can't build manual region"
            << QJsonDocument{manualServer.toJsonObject()}.toJson()
            << "- need both IP and CN";
        return {};
    }

    // Build the region
    QSharedPointer<Location> pLocation{new Location{}};
    pLocation->id(manualRegionId);
    pLocation->autoSafe(false); // Never selected automatically
    pLocation->name(manualServer.cn() + ' ' + manualServer.ip());
    // Just a dummy country code, 'ZZ' is reserved to be user-assigned
    pLocation->country(QStringLiteral("ZZ"));
    // Always allow port forwarding to try it, even if the corresponding region
    // says it doesn't offer it
    pLocation->portForward(true);
    pLocation->geoOnly(false);
    const std::vector<QString> &groups = manualServer.serviceGroups().empty() ?
        manualRegionDefaultGroups : manualServer.serviceGroups();
    auto servers = buildServersFromGroups(groups, manualServer.ip(),
                                          manualServer.cn(),
                                          manualServer.openvpnNcpSupport(),
                                          manualRegionId,
                                          groupTemplates);

    // If OpenVPN UDP/TCP ports were set, override the ports from the servers
    // list with the specified ports.
    // If more one server group was specified that offers OpenVpnUdp/OpenVpnTcp,
    // this might create multiple identical servers, since they're all
    // overridden to the same value.  That's fine, the client has no problem
    // with that.
    for(auto &server : servers)
    {
        if(server.hasService(Service::OpenVpnUdp) && !manualServer.openvpnUdpPorts().empty())
            server.servicePorts(Service::OpenVpnUdp, manualServer.openvpnUdpPorts());
        if(server.hasService(Service::OpenVpnTcp) && !manualServer.openvpnTcpPorts().empty())
            server.servicePorts(Service::OpenVpnTcp, manualServer.openvpnTcpPorts());
    }

    // Find the corresponding region if given for meta servers
    auto itCorrespondingLocation = modernLocations.find(manualServer.correspondingRegionId());
    if(itCorrespondingLocation != modernLocations.end())
        copyRegionMetaServers(*itCorrespondingLocation->second, servers);

    pLocation->servers(std::move(servers));
    return pLocation;
}

void applyLatency(Location &location, const LatencyMap &latencies)
{
    // Is there a latency measurement for this location?
    auto itLatency = latencies.find(location.id());
    if(itLatency != latencies.end())
    {
        // Apply the latency measurement.  (Otherwise, the latency is
        // left unset.)
        location.latency(itLatency->second);
    }
}

LocationsById buildModernLocations(const LatencyMap &latencies,
                                   const QJsonObject &regionsObj,
                                   const QJsonArray &shadowsocksObj,
                                   const std::vector<AccountDedicatedIp> &dedicatedIps,
                                   const ManualServer &manualServer)
{
    // Build template Server objects for each "group" given in the regions list.
    // These will be used later to construct the actual servers by filling in
    // an ID and common name.
    std::unordered_map<QString, Server> groupTemplates;
    const auto &groupsObj = regionsObj["groups"].toObject();

    // Group names are in keys, use Qt iterators
    auto itGroup = groupsObj.begin();
    while(itGroup != groupsObj.end())
    {
        Server groupTemplate;
        // Apply the services.  There may be other services that Desktop doesn't
        // use, ignore those.
        for(const auto &service : itGroup.value().toArray())
            applyModernService(service.toObject(), groupTemplate, itGroup.key());

        // Keep groups even if they have no known services.  This prevents
        // spurious "unknown group" warnings, the servers in this group will be
        // ignored.
        groupTemplates[itGroup.key()] = std::move(groupTemplate);
        ++itGroup;
    }

    // Build a map of the Shadowsocks regions so we can look them up by ID
    // efficiently.
    std::unordered_map<QString, QJsonObject> shadowsocksRegions;
    for(const auto &serverVal : shadowsocksObj)
    {
        // As usual, object() yields an empty object if the array contains
        // something that isn't an object, it'll yield an empty value and
        // throw as expected
        const auto &serverObj = serverVal.toObject();
        try
        {
            auto id = json_cast<QString>(serverObj.value(QStringLiteral("region")), HERE);
            shadowsocksRegions.emplace(std::move(id), serverObj);
        }
        catch(const Error &ex)
        {
            qWarning() << "Ignoring Shadowsocks region with invalid region ID:"
                << QJsonDocument{serverObj}.toJson();
        }
    }

    // Now read the locations and use the group templates to build servers
    LocationsById newLocations;
    const auto &regionsArray = regionsObj["regions"].toArray();
    for(const auto &regionValue : regionsArray)
    {
        const auto &regionObj = regionValue.toObject();

        auto pLocation = readModernLocation(regionObj, groupTemplates, shadowsocksRegions);

        if(pLocation)
        {
            applyLatency(*pLocation, latencies);
            newLocations[pLocation->id()] = std::move(pLocation);
        }
        // Failure to load the location is traced by readModernLocation()
    }

    // Build dedicated IP regions
    for(const auto &dip : dedicatedIps)
    {
        auto pLocation = buildDedicatedIpLocation(newLocations, groupTemplates, dip);
        if(pLocation)
        {
            applyLatency(*pLocation, latencies);
            newLocations[pLocation->id()] = std::move(pLocation);
        }
    }

    // Build the manual location if one is specified
    auto pManualLocation = buildManualLocation(newLocations, groupTemplates,
                                               manualServer);
    if(pManualLocation)
    {
        applyLatency(*pManualLocation, latencies);
        newLocations[pManualLocation->id()] = std::move(pManualLocation);
    }

    return newLocations;
}

// Compare two locations or countries to sort them.
// Sorts by latencies first, then country codes, then by IDs.
// The "tiebreaking" fields (country codes / IDs) are fixed to ensure that we
// sort regions the same way in all contexts.
bool compareEntries(const Location &first, const Location &second)
{
    const Optional<double> &firstLatency = first.latency();
    const Optional<double> &secondLatency = second.latency();
    // Unknown latencies sort last
    if(firstLatency && !secondLatency)
        return true;    // *this < other
    if(!firstLatency && secondLatency)
        return false;   // *this > other

    // If the latencies are known and different, compare them
    if(firstLatency && firstLatency.get() != secondLatency.get())
        return firstLatency.get() < secondLatency.get();

    // Otherwise, the latencies are equivalent (both known and
    // equal, or both unknown and unequal)
    // Compare country codes.
    auto countryComparison = first.country().compare(second.country(),
                                                     Qt::CaseSensitivity::CaseInsensitive);
    if(countryComparison != 0)
        return countryComparison < 0;

    // Same latency and country, compare IDs.
    return first.id().compare(second.id(), Qt::CaseSensitivity::CaseInsensitive) < 0;
}

void buildGroupedLocations(const LocationsById &locations,
                           std::vector<CountryLocations> &groupedLocations,
                           std::vector<QSharedPointer<Location>> &dedicatedIpLocations)
{
    // Group the locations by country
    std::unordered_map<QString, std::vector<QSharedPointer<Location>>> countryGroups;
    dedicatedIpLocations.clear();

    for(const auto &locationEntry : locations)
    {
        Q_ASSERT(locationEntry.second);
        if(locationEntry.second->isDedicatedIp())
            dedicatedIpLocations.push_back(locationEntry.second);
        else
        {
            const auto &countryCode = locationEntry.second->country().toLower();
            countryGroups[countryCode].push_back(locationEntry.second);
        }
    }

    // Sort each countries' locations by latency, then id
    auto sortLocations = [](const QSharedPointer<Location> &pFirst,
                            const QSharedPointer<Location> &pSecond)
    {
        Q_ASSERT(pFirst);
        Q_ASSERT(pSecond);

        return compareEntries(*pFirst, *pSecond);
    };

    for(auto &group : countryGroups)
    {
        std::sort(group.second.begin(), group.second.end(), sortLocations);
    }

    // Sort dedicated IP locations in the same way
    std::sort(dedicatedIpLocations.begin(), dedicatedIpLocations.end(), sortLocations);

    // Create country groups from the sorted lists
    groupedLocations.clear();
    groupedLocations.reserve(countryGroups.size());
    for(const auto &group : countryGroups)
    {
        groupedLocations.push_back({});
        groupedLocations.back().locations(group.second);
    }

    // Sort the countries by their lowest latency
    std::sort(groupedLocations.begin(), groupedLocations.end(),
        [](const auto &first, const auto &second)
        {
            // Consequence of above; groupedLocations created with at least 1 location
            Q_ASSERT(!first.locations().empty());
            Q_ASSERT(!second.locations().empty());
            // Sort by the lowest latency for each country, then country code if
            // the latencies are the same
            const auto &pFirstNearest = first.locations().front();
            const auto &pSecondNearest = second.locations().front();

            Q_ASSERT(pFirstNearest);
            Q_ASSERT(pSecondNearest);
            return compareEntries(*pFirstNearest, *pSecondNearest);
        });
}

NearestLocations::NearestLocations(const LocationsById &allLocations)
{
    _locations.reserve(allLocations.size());
    for(const auto &locationEntry : allLocations)
        _locations.push_back(locationEntry.second);
    std::sort(_locations.begin(), _locations.end(),
                   [](const auto &pFirst, const auto &pSecond) {
                       Q_ASSERT(pFirst);
                       Q_ASSERT(pSecond);

                       return compareEntries(*pFirst, *pSecond);
                   });
}

QSharedPointer<Location> NearestLocations::getNearestSafeVpnLocation(bool portForward) const
{
    // If port forwarding is on, then find fastest server that supports port forwarding
    if(portForward)
    {
        auto result = getBestMatchingLocation([](const Location &loc){return loc.portForward();});
        if(result)
            return result;
    }

    // otherwise just find the best non-PF server
    return getBestLocation();
}
