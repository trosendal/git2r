% Generated by roxygen2 (4.0.0): do not edit by hand
\docType{methods}
\name{is.empty}
\alias{is.empty}
\alias{is.empty,git_repository-method}
\title{Check if repository is empty}
\usage{
is.empty(object)

\S4method{is.empty}{git_repository}(object)
}
\arguments{
\item{object}{The \code{object} to check if it's a empty repository}
}
\value{
TRUE or FALSE
}
\description{
Check if repository is empty
}
\examples{
\dontrun{
## Open an existing repository
repo <- repository("path/to/git2r")

## Check if it's an empty repository
is.empty(repo)
}
}
\keyword{methods}

