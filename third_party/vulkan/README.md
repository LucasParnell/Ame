Khronos Vulkan-Headers SDK 1.3.296.0, commit
`29f979ee5aa58b7b005f805ea8df7a855c39ff37`.
Source: https://github.com/KhronosGroup/Vulkan-Headers

The core, platform and video headers and XML registry are unmodified copies.
The small `vulkan.h` wrapper selects the platform-independent API for Ame.
`../../scripts/vulkan/generate-frontend.py` consumes this registry to generate
native/guest Vulkan 1.0 marshalling metadata and entry points. The registry and
headers are build inputs, independent of Mesa's build output.
