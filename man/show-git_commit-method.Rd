% Generated by roxygen2 (4.0.0): do not edit by hand
\docType{methods}
\name{show,git_commit-method}
\alias{show,git_commit-method}
\alias{show,git_commit-methods}
\title{Brief summary of commit}
\usage{
\S4method{show}{git_commit}(object)
}
\arguments{
\item{object}{The commit \code{object}}
}
\value{
None (invisible 'NULL').
}
\description{
Brief summary of commit
}
\examples{
\dontrun{
## Open an existing repository
repo <- repository("path/to/git2r")

## Brief summary of commit in repository
commits(repo)[[1]]
}
}
\keyword{methods}

