// Phase 4: Streaming trace hooks for model streaming pipeline
// Hooks ScanSectorList, ScanBigBuildingList, FrustumTriangle, SetupEntityVisibility, RequestModelFlags
#pragma once

class PS2Runtime;

namespace vcs::streaming_trace
{
    // Settings: stream_trace.enabled, stream_trace.rate_limit
    void registerSettings();

    // Install hooks on the streaming functions
    void install(PS2Runtime &runtime);
}