#pragma once

// During the WebRTC migration, TcpChannel now lives in signaling/.
// This header re-exports it so that existing transport/ consumers
// continue to compile without changes until Task 3 removes transport/.

#include "signaling/tcp_channel.h"
