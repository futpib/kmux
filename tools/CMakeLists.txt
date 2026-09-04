
### konsoleprofile command-line tool
install(PROGRAMS kmuxprofile DESTINATION ${KDE_INSTALL_BINDIR})

add_executable(kmux-ssh-askpass kmux-ssh-askpass.cpp)
target_link_libraries(kmux-ssh-askpass PRIVATE Qt6::Widgets KF6::I18n)
install(TARGETS kmux-ssh-askpass ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

add_subdirectory( uni2characterwidth )
