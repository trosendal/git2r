## git2r, R bindings to the libgit2 library.
## Copyright (C) 2013-2014  Stefan Widgren
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, version 2 of the License.
##
## git2r is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <http://www.gnu.org/licenses/>.

library(git2r)

##
## Create a directory in tempdir
##
path <- tempfile(pattern="git2r-")
dir.create(path)

##
## Check that open an invalid repository fails
##
tools::assertError(repository(path))

##
## Initialize a repository
##
repo <- init(path)

##
## Check that the state of the repository
##
stopifnot(identical(is.bare(repo), FALSE))
stopifnot(identical(is.empty(repo), TRUE))
stopifnot(identical(branches(repo), list()))
stopifnot(identical(commits(repo), list()))
stopifnot(identical(head(repo), NULL))

##
## Cleanup
##
unlink(path, recursive=TRUE)
