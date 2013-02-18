/*
 * Copyright (C) 2012, 2013 Red Hat, Inc.
 * Written by Florian Weimer <fweimer@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Miscellaneous operating system interfaces.

#pragma once

#include <string>

// Returns true if PATH is a directory.
bool is_directory(const char *path);

// Returns the path to the home directory.
std::string home_directory();

// Creates all directories in PATH, with MODE (should be 0700 or
// 0777).  Return true if the PATH is a directory (that is, if the
// hierarchy already existed or was created successfully), otherwise
// false.
bool make_directory_hierarchy(const char *path, unsigned mode);