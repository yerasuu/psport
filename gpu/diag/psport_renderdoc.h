// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PS_RENDERDOC_CAPTURE_FRAME=N — RenderDoc captures the frame after the Nth game frame
// presented, through its in-application API, when RenderDoc is already in the process (started
// under `renderdoccmd capture` or with its Vulkan layer). Nothing links against RenderDoc, and
// without the variable nothing is looked up.
#pragma once

namespace PsRenderDoc {
/// Called by the presenter after each game frame it presented.
void OnPresent();
} // namespace PsRenderDoc
