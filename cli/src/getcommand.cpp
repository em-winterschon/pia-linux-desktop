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
#line SOURCE_FILE("getcommand.cpp")

#include "getcommand.h"
#include "output.h"
#include "cliclient.h"
#include "settings/daemonstate.h"
#include "settings/daemonsettings.h"
#include "vpnstate.h"
#include <map>

namespace GetSetType
{
    const QString connectionState{QStringLiteral("connectionstate")};
    const QString debugLogging{QStringLiteral("debuglogging")};
    const QString portForward{QStringLiteral("portforward")};
    const QString requestPortForward{QStringLiteral("requestportforward")};
    const QString protocol{QStringLiteral("protocol")};
    const QString region{QStringLiteral("region")};
    const QString regions{QStringLiteral("regions")};
    const QString vpnIp{QStringLiteral("vpnip")};
    const QString pubIp{QStringLiteral("pubip")};
    const QString allowLAN{ QStringLiteral("allowlan") };
    const QString daemonState{QStringLiteral("daemon-state")};
    const QString daemonSettings{QStringLiteral("daemon-settings")};
    const QString daemonData{QStringLiteral("daemon-data")};
    const QString daemonAccount{QStringLiteral("daemon-account")};
}

namespace GetSetValue
{
    const QString booleanTrue{QStringLiteral("true")};
    const QString booleanFalse{QStringLiteral("false")};
    // Get the display string for a boolean setting value
    const QString &getBooleanText(bool value)
    {
        return value ? booleanTrue : booleanFalse;
    }
    // Get the boolean value for a setting passed on the command line.  If the
    // value is not valid, prints an error and throws.
    bool parseBooleanParam(const QString &param)
    {
        if(param == booleanTrue)
            return true;
        if(param == booleanFalse)
            return false;
        errln() << "Unexpected boolean value:" << param << "- expected"
            << booleanTrue << "or" << booleanFalse;
        throw Error{HERE, Error::Code::CliInvalidArgs};
    }
    const QString locationAuto{QStringLiteral("auto")};
    // Get the CLI name used to represent a region.  nullptr is interpreted as
    // "auto".
    QString getRegionCliName(const QSharedPointer<Location> &pLocation)
    {
        if(!pLocation)
            return locationAuto;

        QString name;
        if(pLocation->isDedicatedIp())
        {
            name = QStringLiteral("dedicated-") + pLocation->name() +
                QStringLiteral("-") + pLocation->dedicatedIp();
        }
        else
            name = pLocation->name();
        // Avoid an empty name, just in case
        if(name.isEmpty())
            name = pLocation->id();

        name = name.toLower();
        // Replace any whitespace characters with '-'
        for(int i=0; i<name.length(); ++i)
        {
            if(name[i].isSpace())
                name.replace(i, 1, '-');
        }

        qInfo() << "Location" << pLocation->id() << "/" << pLocation->name()
            << "->" << name;
        return name;
    }

    // Match the location specified on the command line to the daemon's location
    // list.  Returns the location ID if a match is found (or "auto").
    //
    // If no match is found, returns an empty string.
    QString matchSpecifiedLocation(const DaemonState &state, const QString &location)
    {
        if(location == GetSetValue::locationAuto)
            return GetSetValue::locationAuto;

        // This an O(N) lookup, but since we just run once there's no point to
        // building a better representation of the data just to use it once and
        // throw it away.
        for(const auto &knownLocation : state.availableLocations())
        {
            if(knownLocation.second && location == GetSetValue::getRegionCliName(knownLocation.second))
                return knownLocation.second->id();
        }

        qWarning() << "No match found for specified location:" << location;
        return {};
    }
}

namespace
{
    // Get the list of value names for an enumeration from QMetaEnum
    // A "special" placeholder value can also be included at the beginning of
    // the list
    template<class Enum>
    QStringList qEnumValues(const QString &special = {})
    {
        QStringList values;
        auto meta = QMetaEnum::fromType<Enum>();
        values.reserve(meta.keyCount() + (special.isEmpty() ? 0 : 1));
        if(!special.isEmpty())
            values.push_back(special);
        for(int i=0; i<meta.keyCount(); ++i)
            values.push_back(QString{meta.key(i)});
        return values;
    }

    // The supported types for 'get'/'monitor' have help text and optionally a
    // list of values.
    struct SupportedType
    {
        QString _description;
        QStringList _values;
    };

    // Types supported by 'monitor' (and consequently 'get' too).
    // Used to validate the type specified and to print descriptions for types
    // in help.  (Ordered map so the types are printed alphabetically)
    const std::map<QString, SupportedType> _monitorSupportedTypes
    {
        // VpnState::State has a lot of subtle, rarely-occurring values, so it's
        // important to ensure that the possible values are displayed in help
        {GetSetType::connectionState, {QStringLiteral("VPN connection state"),
            qEnumValues<VpnState::State>()}},
        {GetSetType::debugLogging, {QStringLiteral("State of debug logging setting"), {}}},
        {GetSetType::portForward, {QStringLiteral("Forwarded port number if available, or the status of the request to forward a port"),
            qEnumValues<DaemonState::PortForwardState>(QStringLiteral("[forwarded port]"))}},
        {GetSetType::requestPortForward, {QStringLiteral("Whether a forwarded port will be requested on the next connection attempt"), {}}},
        {GetSetType::protocol, {QStringLiteral("VPN connection protocol"), DaemonSettings::choices_method()}},
        {GetSetType::vpnIp, {QStringLiteral("Current VPN IP address"), {}}},
        {GetSetType::pubIp, {QStringLiteral("Current public IP address"), {}}},
        {GetSetType::allowLAN, {QStringLiteral("Whether to allow LAN traffic"), {}}},
        {GetSetType::region, {QStringLiteral("Currently selected region (or \"auto\")"), {}}}
    };


    const std::map<QString, SupportedType> _dumpSupportedTypes
    {
        {GetSetType::daemonState, {QStringLiteral("Internal state of the daemon"), {}}},
        {GetSetType::daemonSettings, {QStringLiteral("Internal settings of the daemon"), {}}},
        {GetSetType::daemonData, {QStringLiteral("Cached data of the daemon"), {}}},
        {GetSetType::daemonAccount, {QStringLiteral("Account status"), {}}}
    };

    // 'regions' is only supported by 'get', not 'monitor'.
    std::map<QString, SupportedType> buildGetSupportedTypes()
    {
        auto types = _monitorSupportedTypes;
        types.insert({GetSetType::regions, {QStringLiteral("List all available regions"), {}}});
        return types;
    }
    const std::map<QString, SupportedType> _getSupportedTypes{buildGetSupportedTypes()};

    void printSupportedTypes(const std::map<QString, SupportedType> &types)
    {
        outln() << "Available types:";
        OutputIndent indent{2};
        for(const auto &type : types)
        {
            outln() << "-" << type.first << "-" << type.second._description;
            if(!type.second._values.isEmpty())
            {
                OutputIndent indent{2};
                outln() << "values:" << type.second._values.join(QStringLiteral(", "));
            }
        }
    }

    class ValuePrinter : public QObject
    {
    public:
        static QString renderLocation(const QSharedPointer<Location> &pLocation);
        static QString renderValue(CliClient &client, const QString &type);

    public:
        ValuePrinter(CliClient &client, QString type);

    private:
        template<class MonitorFunctor>
        void connectValueSignals(CliClient &client, MonitorFunctor func);

    private:
        const QString _type;
        // The last value printed - used when monitoring to avoid printing
        // duplicate changes.  Empty if no value has been printed yet.
        QString _lastValue;
    };

    ValuePrinter::ValuePrinter(CliClient &client, QString type)
        : _type{std::move(type)}
    {
        // We want 'monitor' to print the initial value when establishing a
        // connection.
        // If the initial value isn't the same as the default, a change is
        // emitted when receiving the first data (before connectedChanged is
        // emitted), which causes us to print it.
        // However, if the initial value is the default, we don't receive any
        // change.  If the connection is established and we haven't printed
        // anything yet, print the initial value.
        QObject::connect(&client, &CliClient::firstConnected, this, [this, &client]()
        {
            if(_lastValue.isEmpty())
            {
                _lastValue = renderValue(client, _type);
                outln() << _lastValue;
            }
        });

        connectValueSignals(client, [&client, this]()
        {
            QString newValue = renderValue(client, _type);
            // Print only if the value has actually changed
            // (particularly important for 'monitor region', since the
            // chosenLocation property also contains the region latency and
            // metadata)
            if(newValue != _lastValue)
            {
                _lastValue = std::move(newValue);
                outln() << _lastValue;
            }
        });
    }

    // Render a line representing a location, for either "get location" or
    // "get locations".  nullptr is interpreted as "auto".
    QString ValuePrinter::renderLocation(const QSharedPointer<Location> &pLocation)
    {
        return GetSetValue::getRegionCliName(pLocation);
    }

    QString ValuePrinter::renderValue(CliClient &client, const QString &type)
    {
        if(type == GetSetType::connectionState)
        {
            return client.connection().state.connectionState();
        }
        else if(type == GetSetType::debugLogging)
        {
            // The debugLogging setting is actually an arbitrary set of filters,
            // but any non-null value (even an empty array) enables disk logging
            // and is considered "on".
            bool enabled = !client.connection().settings.debugLogging().isNull();
            return GetSetValue::getBooleanText(enabled);
        }
        else if(type == GetSetType::portForward)
        {
            int forwardedPort = client.connection().state.forwardedPort();
            // For special values, write the state name
            const auto &metaEnum = QMetaEnum::fromType<DaemonState::PortForwardState>();
            const char *name = metaEnum.valueToKey(forwardedPort);
            if(name)
                return name;
            return QString::number(forwardedPort);
        }
        else if(type == GetSetType::requestPortForward)
        {
            bool enabled = client.connection().settings.portForward();
            return GetSetValue::getBooleanText(enabled);
        }
        else if(type == GetSetType::protocol)
        {
            return client.connection().settings.method();
        }
        else if(type == GetSetType::region)
        {
            // Print the chosen location from DaemonState; this includes the
            // name and results in invalid choices being treated as 'auto'
            // (which is what the daemon does)
            return renderLocation(client.connection().state.vpnLocations().chosenLocation());
        }
        else if(type == GetSetType::vpnIp)
        {
            const auto &ip = client.connection().state.externalVpnIp();
            // If the address isn't known, print Unknown
            if(ip.isEmpty())
                return QStringLiteral("Unknown");
            return ip;
        }
        else if(type == GetSetType::pubIp)
        {
            const auto &ip = client.connection().state.externalIp();
            // If the address isn't known, print Unknown
            if(ip.isEmpty())
                return QStringLiteral("Unknown");
            return ip;
        }
        else if(type == GetSetType::daemonSettings)
        {
            QJsonDocument document;
            document.setObject(client.connection().settings.toJsonObject());
            return document.toJson(QJsonDocument::Indented);
        }
        else if(type == GetSetType::daemonState)
        {
            QJsonDocument document;
            document.setObject(client.connection().state.toJsonObject());
            return document.toJson(QJsonDocument::Indented);
        }
        else if(type == GetSetType::daemonData)
        {
            QJsonDocument document;
            document.setObject(client.connection().data.toJsonObject());
            return document.toJson(QJsonDocument::Indented);
        }
        else if(type == GetSetType::daemonAccount)
        {
            QJsonDocument document;
            document.setObject(client.connection().account.toJsonObject());
            return document.toJson(QJsonDocument::Indented);
        }
        else if (type == GetSetType::allowLAN)
        {
            // Print the status of allowLAN
            bool enabled = client.connection().settings.allowLAN();
            return GetSetValue::getBooleanText(enabled);
        }
        else
        {
            // exec() prevents this by checking the type with checkParams()
            Q_ASSERT(false);
            return {};
        }
    }

    // Connect the property change signals corresponding to the properties used
    // by printValue
    template<class MonitorFunctor>
    void ValuePrinter::connectValueSignals(CliClient &client, MonitorFunctor func)
    {
        if(_type == GetSetType::connectionState)
        {
            QObject::connect(&client.connection().state, &DaemonState::connectionStateChanged,
                             this, func);
        }
        else if(_type  == GetSetType::debugLogging)
        {
            QObject::connect(&client.connection().settings, &DaemonSettings::debugLoggingChanged,
                             this, func);
        }
        else if(_type  == GetSetType::portForward)
        {
            QObject::connect(&client.connection().state, &DaemonState::forwardedPortChanged,
                             this, func);
        }
        else if(_type == GetSetType::protocol)
        {
            QObject::connect(&client.connection().settings, &DaemonSettings::methodChanged,
                             this, func);
        }
        else if(_type == GetSetType::requestPortForward)
        {
            QObject::connect(&client.connection().settings, &DaemonSettings::portForwardChanged,
                             this, func);
        }
        else if(_type  == GetSetType::region)
        {
            // The chosen location is one part of the vpnLocations object, when
            // it changes the entire object emits a change.
            QObject::connect(&client.connection().state, &DaemonState::vpnLocationsChanged,
                             this, func);
        }
        else if(_type  == GetSetType::regions)
        {
            QObject::connect(&client.connection().state, &DaemonState::groupedLocationsChanged,
                             this, func);
        }
        else if(_type  == GetSetType::vpnIp)
        {
            QObject::connect(&client.connection().state, &DaemonState::externalVpnIpChanged,
                             this, func);
        }
        else if(_type  == GetSetType::pubIp)
        {
            QObject::connect(&client.connection().state, &DaemonState::externalIpChanged,
                             this, func);
        }
        else if (_type == GetSetType::allowLAN)
        {
            QObject::connect(&client.connection().settings, &DaemonSettings::allowLANChanged,
                             this, func);
        }
        else
        {
            // exec() prevents this by checking the type with checkParams()
            Q_ASSERT(false);
        }
    }

    // Check get/monitor parameters.  Prints an error and throws if the
    // parameters are not valid
    void checkParams(const QStringList &params, const std::map<QString, SupportedType> &types)
    {
        if(params.length() != 2)
        {
            errln() << "Usage:" << params[0] << "<type>";
            throw Error{HERE, Error::Code::CliInvalidArgs};
        }

        if(types.count(params[1]) == 0)
        {
            errln() << "Unknown type:" << params[1];
            throw Error{HERE, Error::Code::CliInvalidArgs};
        }
    }
}

void GetCommand::printHelp(const QString &name)
{
    outln() << "usage:" << name << "<type>";
    outln() << "Get information from the PIA daemon.";
    printSupportedTypes(_getSupportedTypes);
}

int GetCommand::exec(const QStringList &params, QCoreApplication &app)
{
    checkParams(params, _getSupportedTypes);

    CliClient client;
    CliTimeout timeout{app};
    QObject localConnState{};

    QObject::connect(&client, &CliClient::firstConnected, &localConnState, [&]()
    {
        // Handle types only supported by 'get' specifically
        if(params[1] == GetSetType::regions)
        {
            // Print locations in the default order they're listed in the
            // client - DIP locations by latency, then normal locations by
            // country and latency
            outln() << ValuePrinter::renderLocation({});  // Auto

            const auto &dedicatedIpLocations = client.connection().state.dedicatedIpLocations();
            for(const auto &pDip : dedicatedIpLocations)
            {
                if(pDip)
                    outln() << ValuePrinter::renderLocation(pDip);
            }

            const auto &groupedLocations = client.connection().state.groupedLocations();
            for(const auto &country : groupedLocations)
            {
                for(const auto &pLocation : country.locations())
                {
                    if(pLocation)
                        outln() << ValuePrinter::renderLocation(pLocation);
                }
            }
        }
        else
            outln() << ValuePrinter::renderValue(client, params[1]);

        app.exit(CliExitCode::Success);
    });

    return app.exec();
}


void MonitorCommand::printHelp(const QString &name)
{
    outln() << "usage:" << name << "<type>";
    outln() << "Monitors the PIA daemon for changes in a specific setting or state value.";
    outln() << "When a connection is established, the current value is printed.";
    outln() << "When a change is received, the new value is printed.";
    printSupportedTypes(_monitorSupportedTypes);
}

int MonitorCommand::exec(const QStringList &params, QCoreApplication &app)
{
    checkParams(params, _monitorSupportedTypes);

    CliClient client;
    ValuePrinter printer{client, params[1]};

    return app.exec();
}


void DumpCommand::printHelp(const QString &name)
{
    outln() << "usage:" << name << "<type>";
    outln() << "Prints internal information about the running PIA Daemon";
    printSupportedTypes(_dumpSupportedTypes);
}

int DumpCommand::exec(const QStringList &params, QCoreApplication &app)
{
    checkParams(params, _dumpSupportedTypes);

    CliClient client;
    CliTimeout timeout{app};
    QObject localConnState{};
    QObject::connect(&client, &CliClient::firstConnected, &localConnState, [&]()
    {
        outln() << ValuePrinter::renderValue(client, params[1]);
        app.exit(CliExitCode::Success);
    });

    return app.exec();
}
