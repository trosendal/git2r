% Generated by roxygen2 (4.0.0): do not edit by hand
\docType{methods}
\name{is.bare}
\alias{is.bare}
\alias{is.bare,git_repository-method}
\title{Check if repository is bare}
\usage{
is.bare(object)

\S4method{is.bare}{git_repository}(object)
}
\arguments{
\item{object}{The \code{object} to check if it's a bare repository}
}
\value{
TRUE if bare repository, else FALSE
}
\description{
Check if repository is bare
}
\examples{
\dontrun{
## Open an existing repository
repo <- repository("path/to/git2r")

## Check if it's a bare repository
is.bare(repo)
}
}
\keyword{methods}

