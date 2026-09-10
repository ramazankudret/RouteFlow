# RouteFlow, built from source and shipped as two binaries.
#
# The tree has no third-party dependencies on purpose (ARCHITECTURE §3), so the
# build stage needs a compiler and CMake and nothing else, and the runtime stage
# needs a libstdc++ and nothing else. That is the whole reason this image is
# small: there is nothing in it that the project did not write.
#
#   docker build -t routeflow .
#   docker run --rm routeflow routeflow-router --help
#
# One image, both binaries. Which one you want is the command you pass:
# `routeflow-agent` beside each engine, `routeflow-router` once for the cluster.

FROM debian:12-slim AS build
# curl and python3 are for the test suite, not for the build: snapshot_auth
# drives the built binaries over a real socket and reads the JSON back. They
# stay in this stage and never reach the runtime image.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      g++ cmake make ca-certificates curl python3 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY common/ common/
COPY agent/ agent/
COPY router/ router/
COPY tests/ tests/
# The socket test needs a node to talk to and a router config to start from.
COPY bench/profiles/ bench/profiles/
COPY bench/router.json bench/router.json

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 && cmake --build build -j"$(nproc)"

# The suites run here rather than in CI-only, so an image that builds but does
# not work cannot be produced quietly. snapshot_auth drives the real binaries
# over a real socket, which is the half that matters for a shipped artifact.
RUN cd build && ctest --output-on-failure

FROM debian:12-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --create-home --uid 10001 routeflow

COPY --from=build /src/build/routeflow-agent  /usr/local/bin/routeflow-agent
COPY --from=build /src/build/routeflow-router /usr/local/bin/routeflow-router
# The operator console is four static files the router serves itself.
COPY ui/ /usr/local/share/routeflow/ui/

USER routeflow
WORKDIR /home/routeflow

# The console lives at an absolute path in the image rather than beside the
# working directory the way it does in a source tree, and the config layer reads
# flags and files only -- no environment variables -- so this has to be passed
# on the command line:
#
#   docker run -p 8970:8970 routeflow routeflow-router \
#       --nodes /etc/routeflow/nodes.json \
#       --ui.dir /usr/local/share/routeflow/ui
EXPOSE 8970 8971
ENTRYPOINT []
CMD ["routeflow-router", "--help"]
