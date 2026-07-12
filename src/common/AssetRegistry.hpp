// Thread-safe set of live model pointers, for a module to mark "this instance came from my
// pipeline" and query it later from a draw-time hook. DLL-only: live runtime pointers only exist
// once a model is loaded in the injected process, so this has no host form.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <shared_mutex>
#include <unordered_set>

namespace wxl::modern::assets::common
{
    /**
     * @brief Thread-safe registry of live model pointers a module reshaped or otherwise owns.
     *
     * A load hook Remember()s a model; a free/replace path Forget()s it (guarding against a new
     * model reusing a freed model's address); a draw or finalize hook queries Contains() to scope
     * itself to instances this module actually touched.
     */
    class AssetRegistry
    {
    public:
        /** @brief Registers a model pointer. @param model Runtime model pointer. */
        void Remember(void* model)
        {
            std::unique_lock lock(mutex_);
            set_.insert(model);
        }

        /** @brief Drops a model pointer. @param model Runtime model pointer. */
        void Forget(void* model)
        {
            std::unique_lock lock(mutex_);
            set_.erase(model);
        }

        /**
         * @brief Queries whether a model pointer is registered.
         * @param model Runtime model pointer.
         * @return true if Remember() was called for this pointer and Forget() has not since.
         */
        bool Contains(void* model) const
        {
            std::shared_lock lock(mutex_);
            return set_.find(model) != set_.end();
        }

    private:
        mutable std::shared_mutex mutex_;
        std::unordered_set<void*> set_;
    };
}
