#include "../src/modules/visual/BlockEventQueue.hpp"

#include <cassert>
#include <string_view>

using bedaim::visual::BlockEventQueue;
using bedaim::visual::BlockEventPosition;

int main() {
    {
        BlockEventQueue<4, 8> queue;
        const BlockEventPosition position{12, 64, -9};
        const auto first = queue.enqueue(reinterpret_cast<void*>(0x1000), position);
        assert(first.inserted && !first.merged && !first.droppedOldest);
        const auto duplicate = queue.enqueue(reinterpret_cast<void*>(0x2000), position, "tile.chest");
        assert(!duplicate.inserted && duplicate.merged && !duplicate.droppedOldest);
        assert(queue.size() == 1);

        auto candidates = queue.drain(4);
        assert(candidates.size() == 1);
        assert(candidates[0].source == reinterpret_cast<void*>(0x2000));
        assert(candidates[0].position == position);
        assert(candidates[0].blockNameLength == 10);
        assert(std::string_view(candidates[0].blockName.data(), candidates[0].blockNameLength) == "tile.chest");
        assert(queue.size() == 0);
    }

    {
        BlockEventQueue<2, 4> queue;
        const BlockEventPosition first{1, 2, 3};
        const BlockEventPosition second{4, 5, 6};
        const BlockEventPosition third{7, 8, 9};
        assert(queue.enqueue(nullptr, first).inserted);
        assert(queue.enqueue(nullptr, second).inserted);
        const auto overflow = queue.enqueue(nullptr, third);
        assert(overflow.inserted && overflow.droppedOldest);
        assert(queue.size() == 2);

        auto candidates = queue.drain(2);
        assert(candidates.size() == 2);
        assert(candidates[0].position == second);
        assert(candidates[1].position == third);
    }

    {
        BlockEventQueue<3, 8> queue;
        const BlockEventPosition first{0, 0, 0};
        const BlockEventPosition second{1, 0, 0};
        const BlockEventPosition third{2, 0, 0};
        assert(queue.enqueue(nullptr, first).inserted);
        assert(queue.enqueue(nullptr, second).inserted);
        auto one = queue.drain(1);
        assert(one.size() == 1 && one[0].position == first);
        assert(queue.enqueue(nullptr, third).inserted);
        assert(queue.enqueue(nullptr, first).inserted); // drained key can be reinserted
        auto rest = queue.drain(3);
        assert(rest.size() == 3);
        assert(rest[0].position == second);
        assert(rest[1].position == third);
        assert(rest[2].position == first);
    }

    {
        BlockEventQueue<4, 8> queue;
        const BlockEventPosition position{9, 9, 9};
        assert(queue.enqueue(nullptr, position, "tile.chest").inserted);
        queue.clear();
        assert(queue.size() == 0);
        assert(queue.enqueue(nullptr, position).inserted);
        assert(queue.drain(1).size() == 1);
    }

    return 0;
}
