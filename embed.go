package saucer

import "embed"

// Source embeds the saucer C++ source files for Go vendoring.
//
//go:embed CMakeLists.txt
//go:embed cmake/*.cmake
//go:embed cmake/toolchain/*.cmake
//go:embed cmake/toolchain/*.hpp
//go:embed include/saucer/*.hpp
//go:embed include/saucer/*.inl
//go:embed include/saucer/error/*.hpp
//go:embed include/saucer/error/*.inl
//go:embed include/saucer/modules/*.hpp
//go:embed include/saucer/modules/stable/*.hpp
//go:embed include/saucer/serializers/*.hpp
//go:embed include/saucer/serializers/*.inl
//go:embed include/saucer/serializers/format/*.hpp
//go:embed include/saucer/serializers/format/*.inl
//go:embed include/saucer/serializers/glaze/*.hpp
//go:embed include/saucer/serializers/glaze/*.inl
//go:embed include/saucer/serializers/rflpp/*.hpp
//go:embed include/saucer/serializers/rflpp/*.inl
//go:embed include/saucer/stash/*.hpp
//go:embed include/saucer/stash/*.inl
//go:embed include/saucer/traits/*.hpp
//go:embed include/saucer/traits/*.inl
//go:embed include/saucer/utils/*.hpp
//go:embed include/saucer/utils/*.inl
//go:embed private/saucer/*.hpp
//go:embed private/saucer/*.inl
//go:embed src/*.cpp
//go:embed src/*.mm
//go:embed src/module/*.cpp
//go:embed src/module/*.mm
//go:embed template/*.in
var Source embed.FS
