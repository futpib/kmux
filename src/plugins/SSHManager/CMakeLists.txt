
kcoreaddons_add_plugin(kmux_sshmanagerplugin
SOURCES
    sshmanagerplugin.cpp
    sshmanagerpluginwidget.cpp
    sshmanagermodel.cpp
    sshmanagerfiltermodel.cpp
    sshtreeview.cpp
INSTALL_NAMESPACE
    "kmuxplugins"
)

ki18n_wrap_ui(kmux_sshmanagerplugin sshwidget.ui)

ecm_qt_declare_logging_category(kmux_sshmanagerplugin
    HEADER sshmanagerplugindebug.h
    IDENTIFIER SshManagerPluginDebug
    CATEGORY_NAME org.kde.konsole.plugin.sshmanager
    DESCRIPTION "Konsole plugin (SSH Manager)"
    EXPORT KONSOLE
)

configure_file(konsole_sshmanager.in.json konsole_sshmanager.json)
target_link_libraries(kmux_sshmanagerplugin
    konsoleprivate
    konsoleapp
    KF6::CoreAddons
    KF6::Crash
    KF6::I18n
)
