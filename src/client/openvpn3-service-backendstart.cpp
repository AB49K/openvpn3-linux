//  OpenVPN 3 Linux client -- Next generation OpenVPN client
//
//  Copyright (C) 2017      OpenVPN Inc. <sales@openvpn.net>
//  Copyright (C) 2017      David Sommerseth <davids@openvpn.net>
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, version 3 of the License
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <iostream>

#include "config.h"
#include "dbus/core.hpp"
#include "log/dbus-log.hpp"
#include "common/utils.hpp"

using namespace openvpn;

class BackendManagerSignals : public LogSender
{
public:
    BackendManagerSignals(GDBusConnection *conn, LogGroup lgroup, std::string object_path)
            : LogSender(conn, lgroup, OpenVPN3DBus_interf_backends, object_path)
    {
    }

    virtual ~BackendManagerSignals()
    {
    }

    void LogFATAL(std::string msg)
    {
        Log(log_group, LogCategory::FATAL, msg);
        StatusChange(StatusMajor::SESSION, StatusMinor::PROC_KILLED, msg);
        abort();
    }


    void Debug(std::string msg)
    {
            LogSender::Debug(msg);
    }

    void Debug(std::string busname, std::string path, pid_t pid, std::string msg)
    {
            std::stringstream debug;
            debug << "pid=" << std::to_string(pid)
                  << ", busname=" << busname
                  << ", path=" << path
                  << ", message=" << msg;
            LogSender::Debug(debug.str());
    }

    void StatusChange(const StatusMajor major, const StatusMinor minor, std::string msg)
    {
        GVariant *params = g_variant_new("(uus)", (guint) major, (guint) minor, msg.c_str());
        Send("StatusChange", params);
    }

    void StatusChange(const StatusMajor major, const StatusMinor minor)
    {
        GVariant *params = g_variant_new("(uus)", (guint) major, (guint) minor, "");
        Send("StatusChange", params);
    }
};


class BackendManagerObject : public DBusObject
{
public:
    BackendManagerObject(GDBusConnection *dbuscon, const std::string busname, const std::string objpath)
        : DBusObject(objpath),
          dbuscon(dbuscon),
          signal(nullptr)
    {
        signal = new BackendManagerSignals(dbuscon, LogGroup::BACKENDSTART, objpath);

        std::stringstream introspection_xml;
        introspection_xml << "<node name='" << objpath << "'>"
                          << "    <interface name='" << OpenVPN3DBus_interf_backends << "'>"
                          << "        <method name='StartClient'>"
                          << "          <arg type='s' name='token' direction='in'/>"
                          << "          <arg type='u' name='pid' direction='out'/>"
                          << "        </method>"
                          << signal->GetLogIntrospection()
                          << "    </interface>"
                          << "</node>";
        ParseIntrospectionXML(introspection_xml);

        signal->Debug("BackendManagerObject registered on '" + OpenVPN3DBus_interf_backends + "': "
                      + objpath);
    }

    ~BackendManagerObject()
    {
        signal->LogInfo("Shutting down");
        delete signal;
        RemoveObject(dbuscon);
    }

    void OpenLogFile(std::string filename)
    {
        signal->OpenLogFile(filename);
    }


    void callback_method_call(GDBusConnection *conn,
                              const std::string sender,
                              const std::string obj_path,
                              const std::string intf_name,
                              const std::string method_name,
                              GVariant *params,
                              GDBusMethodInvocation *invoc)
    {
        if ("StartClient" == method_name)
        {
            IdleCheck_UpdateTimestamp();

            // Retrieve the configuration path for the tunnel
            // from the request
            gchar *token;
            g_variant_get (params, "(s)", &token);
            pid_t backend_pid = start_backend_process(token);
            if (-1 == backend_pid)
            {
                GError *err = g_dbus_error_new_for_dbus_error("net.openvpn.v3.error.backend",
                                                              "Backend client process died");
                g_dbus_method_invocation_return_gerror(invoc, err);
                g_error_free(err);
                return;
            }
            g_dbus_method_invocation_return_value(invoc, g_variant_new("(u)", backend_pid));
        }
    };


    GVariant * callback_get_property(GDBusConnection *conn,
                                     const std::string sender,
                                     const std::string obj_path,
                                     const std::string intf_name,
                                     const std::string property_name,
                                     GError **error)
    {
        /*
        std::cout << "[BackendManagerObject] get_property(): "
                  << "sender=" << sender
                  << ", object_path=" << obj_path
                  << ", interface=" << intf_name
                  << ", property=" << property_name
                  << std::endl;
        */
        g_set_error (error,
                     G_IO_ERROR,
                     G_IO_ERROR_FAILED,
                     "Unknown property");
        return NULL;
    };


    GVariantBuilder * callback_set_property(GDBusConnection *conn,
                                            const std::string sender,
                                            const std::string obj_path,
                                            const std::string intf_name,
                                            const std::string property_name,
                                            GVariant *value,
                                            GError **error)
    {
        THROW_DBUSEXCEPTION("BackendManagerObject", "set property not implemented");
    }


private:
    GDBusConnection *dbuscon;
    BackendManagerSignals *signal;

    pid_t start_backend_process(char * token)
    {
        pid_t backend_pid = fork();
        if (0 == backend_pid)
        {
            // Child
            char * const client_args[] = {
#ifdef DEBUG_VALGRIND
                (char *) "/usr/bin/valgrind",
                (char *) "--log-file=/tmp/valgrind.log",
#endif
                (char *) LIBEXEC_PATH "/openvpn3-service-client",
                token,
                NULL };
            execve(client_args[0], client_args, NULL);

            // If execve() succeedes, the line below will not be executed at all.
            // So if we come here, there must be an error.
            std::cerr << "** Error starting " << client_args[0] << ": " << strerror(errno) << std::endl;
        }
        else if( backend_pid > 0)
        {
            // Parent

            std::stringstream msg;
            // Wait for the child process to exit, as the client process will fork again
            int rc = -1;
            int w = waitpid(backend_pid, &rc, 0);
            if (-1 == w)
            {
                msg << "Child process ("  << token
                    << ") - pid " << backend_pid
                    << " failed to start as expected (exit code: "
                    << std::to_string(rc) << ")";
                signal->LogError(msg.str());
                return -1;
            }
            return backend_pid;
        }
        throw std::runtime_error("Failed to fork() backend client process");
    }
};


class BackendManagerDBus : public DBus
{
public:
    BackendManagerDBus(GBusType bus_type)
        : DBus(bus_type,
               OpenVPN3DBus_name_backends,
               OpenVPN3DBus_rootp_backends,
               OpenVPN3DBus_interf_backends),
          managobj(nullptr),
          procsig(nullptr),
          logfile("")
    {
    };

    ~BackendManagerDBus()
    {
        delete managobj;

        procsig->ProcessChange(StatusMinor::PROC_STOPPED);
        delete procsig;
    }

    void SetLogFile(std::string filename)
    {
        logfile = filename;
    }

    void callback_bus_acquired()
    {
        managobj = new BackendManagerObject(GetConnection(), GetBusName(), GetRootPath());
        if (!logfile.empty())
        {
            managobj->OpenLogFile(logfile);
        }
        managobj->RegisterObject(GetConnection());

        procsig = new ProcessSignalProducer(GetConnection(),
                                            OpenVPN3DBus_interf_backends,
                                            "BackendManager");
        procsig->ProcessChange(StatusMinor::PROC_STARTED);

        if (idle_checker)
        {
            managobj->IdleCheck_Register(idle_checker);
        }
    };

    void callback_name_acquired(GDBusConnection *conn, std::string busname)
    {
    };

    void callback_name_lost(GDBusConnection *conn, std::string busname)
    {
        THROW_DBUSEXCEPTION("BackendManagerDBus", "Backend Manager's D-Bus name not registered: '" + busname + "'");
    };

private:
    BackendManagerObject * managobj;
    ProcessSignalProducer * procsig;
    std::string logfile;
};



int main(int argc, char **argv)
{
    GMainLoop *main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, stop_handler, main_loop);
    g_unix_signal_add(SIGTERM, stop_handler, main_loop);

    IdleCheck idle_exit(main_loop, std::chrono::minutes(5));
    idle_exit.SetPollTime(std::chrono::seconds(20));

    BackendManagerDBus backendmgr(G_BUS_TYPE_SYSTEM);
    backendmgr.EnableIdleCheck(&idle_exit);
    backendmgr.Setup();

    idle_exit.Enable();
    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);
    idle_exit.Disable();
    idle_exit.Join();

    return 0;
}
