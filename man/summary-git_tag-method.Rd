% Generated by roxygen2 (4.0.0): do not edit by hand
\docType{methods}
\name{summary,git_tag-method}
\alias{summary,git_tag-method}
\alias{summary,git_tag-methods}
\title{Summary of a tag}
\usage{
\S4method{summary}{git_tag}(object, ...)
}
\arguments{
\item{object}{The tag \code{object}}

\item{...}{additional arguments affecting the summary produced.}
}
\value{
None (invisible 'NULL').
}
\description{
Summary of a tag
}
\examples{
\dontrun{
## Open an existing repository
repo <- repository("path/to/git2r")

## Apply summary to each tag in the repository
invisible(lapply(tags(repo), summary))
}
}
\keyword{methods}

