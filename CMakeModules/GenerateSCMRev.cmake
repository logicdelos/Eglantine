# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# generate git/build information
include(GetSCMRev)

function(get_timestamp _var)
    string(TIMESTAMP timestamp UTC)
    set(${_var} "${timestamp}" PARENT_SCOPE)
endfunction()

get_timestamp(BUILD_DATE)

if (DEFINED GIT_RELEASE)
    set(BUILD_VERSION "${GIT_TAG}")
    set(GIT_REFSPEC "${GIT_RELEASE}")
    set(IS_DEV_BUILD false)
else()
    string(SUBSTRING ${GIT_COMMIT} 0 10 BUILD_VERSION)
    set(BUILD_VERSION "${BUILD_VERSION}-${GIT_REFSPEC}")
    set(IS_DEV_BUILD true)
endif()

if (NIGHTLY_BUILD)
    set(IS_NIGHTLY_BUILD true)
else()
    set(IS_NIGHTLY_BUILD false)
endif()

set(GIT_DESC ${BUILD_VERSION})

# Generate cpp with Git revision from template

set(BUILD_AUTO_UPDATE_WEBSITE "https://github.com")
set(BUILD_AUTO_UPDATE_API "api.github.com")
set(BUILD_AUTO_UPDATE_REPO "${EGLANTINE_UPDATE_REPO}")
set(BUILD_RELEASE_PLATFORM "${EGLANTINE_RELEASE_PLATFORM}")
# This is an unofficial fork: the name the program shows for itself says so.
if (NIGHTLY_BUILD)
    set(REPO_NAME "Eglantine (logicdelos fork) Nightly")
else()
    set(REPO_NAME "Eglantine (logicdelos fork)")
endif()

# The version this build reports in the window title, the About dialog and the
# first log line. The cache variable EGLANTINE_FORK_VERSION (CMakeLists.txt)
# decides it. Left empty, the first of these that exists wins:
#   1. the display identity CI wrote (GIT-DISPLAY or DISPLAYVERSION, read in
#      src/common/CMakeLists.txt, already escaped for a C string);
#   2. the tag in the GIT-TAG file, when it is a fork tag (fork-vX.Y.Z);
#   3. `git describe --tags --match "fork-v*"` with the "fork-v" prefix removed,
#      for example 0.1.0 on the tag and 0.1.0-3-g1a2b3c4 three commits after it;
#   4. 0.1.0-dev+<short commit>, when no fork tag is reachable (a shallow clone,
#      a source archive). Without a commit it is plain 0.1.0-dev.
# The value is fixed when CMake configures; re-run CMake after a new commit or tag.
set(FORK_VERSION "${EGLANTINE_FORK_VERSION}")
if (NOT FORK_VERSION AND NOT EGLANTINE_DISPLAY_VERSION)
    if (EXISTS "${CMAKE_SOURCE_DIR}/GIT-TAG" AND GIT_TAG MATCHES "^fork-v[0-9]")
        set(FORK_VERSION "${GIT_TAG}")
    else()
        run_git_command(FORK_VERSION describe --tags --match "fork-v*")
    endif()
    if (FORK_VERSION MATCHES "^fork-v[0-9]" AND NOT FORK_VERSION MATCHES "NOTFOUND")
        string(REGEX REPLACE "^fork-v" "" FORK_VERSION "${FORK_VERSION}")
    else()
        set(FORK_VERSION "0.1.0-dev")
        if (GIT_COMMIT MATCHES "^[0-9a-f]+$")
            string(SUBSTRING "${GIT_COMMIT}" 0 7 FORK_SHORT_COMMIT)
            set(FORK_VERSION "${FORK_VERSION}+${FORK_SHORT_COMMIT}")
        endif()
    endif()
endif()
if (FORK_VERSION)
    # The value ends up inside a C string literal.
    string(REPLACE "\\" "\\\\" FORK_VERSION "${FORK_VERSION}")
    string(REPLACE "\"" "\\\"" FORK_VERSION "${FORK_VERSION}")
    set(EGLANTINE_DISPLAY_VERSION "${FORK_VERSION}")
endif()
message(STATUS "[Eglantine] ${REPO_NAME} ${EGLANTINE_DISPLAY_VERSION}")

# Inside a C string literal, like the version above.
set(UPSTREAM_BASE "${EGLANTINE_UPSTREAM_BASE}")
string(REPLACE "\\" "\\\\" UPSTREAM_BASE "${UPSTREAM_BASE}")
string(REPLACE "\"" "\\\"" UPSTREAM_BASE "${UPSTREAM_BASE}")

set(BUILD_ID ${GIT_REFSPEC})
set(BUILD_FULLNAME "${REPO_NAME} ${BUILD_VERSION} ")
set(CXX_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

configure_file(scm_rev.cpp.in scm_rev.cpp @ONLY)
