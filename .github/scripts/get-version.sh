# GNU extensions for sed are not supported; on Linux, --posix mimics this behaviour
TXS_VERSION=$(sed -ne 's/^#define TXSVERSION "\(.*\)".*$/\1/p' ../src/utilsVersion.h)
echo "TXS_VERSION = ${TXS_VERSION}"
FORK_REVISION=$(cat ../VIM_REVISION)
case "$FORK_REVISION" in
    ""|*[!0-9]*) echo "Invalid VIM_REVISION: expected a nonnegative integer" >&2; return 1 ;;
esac

GIT_HASH=$(git show --no-patch --pretty="%h")
echo "GIT_HASH = ${GIT_HASH}"

GIT_DATE=$(git show --no-patch --pretty="%ci")
echo "GIT_DATE = ${GIT_DATE}"

# Use the triggering fork tag, never an imported upstream tag or another release.
if [ "${GITHUB_REF_TYPE:-}" = "tag" ]; then
    RELEASE_TAG=${GITHUB_REF_NAME}
else
    RELEASE_TAG=$(git describe --tags --match 'texstudio-vim-*' --abbrev=0 2>/dev/null || true)
fi
case "$RELEASE_TAG" in
    texstudio-vim-*) GIT_VERSION=${RELEASE_TAG#texstudio-vim-} ;;
    *) GIT_VERSION=${TXS_VERSION} ;;
esac
# Legacy releases are the r0 baseline; numbered tags carry their own revision.
case "$GIT_VERSION" in
    *-r[0-9]*) ;;
    *) GIT_VERSION="${GIT_VERSION}-r${FORK_REVISION}" ;;
esac
echo "GIT_VERSION = ${GIT_VERSION}"

DATE_HASH=$(date -u +"%Y%m%d%H%M")
echo "DATE_HASH = ${DATE_HASH}"
OS_NAME=$(uname)
echo ${OS_NAME}
if [ "${OS_NAME}" = "Darwin" ]; then
	RELEASE_DATE=$(date -ujf "%Y-%m-%d %H:%M:%S %z" "${GIT_DATE}" "+%Y-%m-%dT%H:%M:%S%z")
else
	RELEASE_DATE=$(date -u +"%Y-%m-%dT%H:%M:%S%z" --date="${GIT_DATE}")
fi
echo "RELEASE_DATE = ${RELEASE_DATE}"

VERSION_NAME="${GIT_VERSION}-${DATE_HASH}-git_${GIT_HASH}"
echo "VERSION_NAME = ${VERSION_NAME}"
