# Keep KDE's translated metadata unchanged in the source tree. Generate the
# fork's installed identity here so upstream translation updates merge cleanly.
function(kmux_brand_metadata source destination)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(STRINGS "${source}" lines ENCODING UTF-8)
    set(branded "")
    foreach(line IN LISTS lines)
        if(source MATCHES "\\.appdata\\.xml$")
            # Rebrand display text, not URLs, credits, or XML attributes.
            if(line MATCHES "^[ \t]*<(name|p|li|caption)[ >]")
                while(line MATCHES ">[^<]*Konsole")
                    string(REGEX REPLACE ">([^<]*)Konsole" ">\\1kmux" line "${line}")
                endwhile()
            endif()
            string(REPLACE ">org.kde.konsole.desktop<" ">org.kde.kmux.desktop<" line "${line}")
            string(REPLACE "<binary>konsole</binary>" "<binary>kmux</binary>" line "${line}")
        else()
            if(line MATCHES "^(Name|Comment)(\\[[^]]+\\])?=")
                string(REPLACE "Konsole" "kmux" line "${line}")
            elseif(line MATCHES "^(Exec|TryExec)=konsole([ \t]|$)")
                string(REGEX REPLACE "^([^=]+=)konsole" "\\1kmux" line "${line}")
            elseif(line MATCHES "^(Icon|IconName)=utilities-terminal$")
                string(REPLACE "utilities-terminal" "kmux" line "${line}")
            elseif(line MATCHES "^Keywords=")
                string(REGEX REPLACE ";+$" "" line "${line}")
                string(APPEND line ";kmux")
            endif()
            string(REPLACE "DesktopEntry=org.kde.konsole" "DesktopEntry=org.kde.kmux" line "${line}")
            string(REPLACE "X-DocPath=konsole/" "X-DocPath=kmux/" line "${line}")
            string(REPLACE "StartupWMClass=konsole" "StartupWMClass=kmux" line "${line}")
            string(REPLACE "X-KDE-Library=konsolepart" "X-KDE-Library=kmuxpart" line "${line}")
            string(REPLACE "runInKonsole" "runInKmux" line "${line}")
        endif()
        string(APPEND branded "${line}\n")
    endforeach()

    # Preserve timestamps when configuration runs without metadata changes.
    if(EXISTS "${destination}")
        file(READ "${destination}" previous)
        if(previous STREQUAL branded)
            return()
        endif()
    endif()
    file(WRITE "${destination}" "${branded}")
endfunction()

foreach(metadata
        org.kde.konsole.appdata.xml
        org.kde.konsole.desktop
        konsole.notifyrc
        konsolepart.desktop
        konsolerun.desktop)
    string(REPLACE "konsole" "kmux" branded_metadata "${metadata}")
    kmux_brand_metadata("${CMAKE_CURRENT_SOURCE_DIR}/${metadata}" "${CMAKE_CURRENT_BINARY_DIR}/${branded_metadata}")
endforeach()
