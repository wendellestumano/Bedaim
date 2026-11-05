# Main Hand Survival trial — Bedrock 26.60

This experimental branch tests a direct copy of the selected Main Hand stack into an empty hotbar slot, with no Bundle or container machine.

The previous direct item-pickup race was explicitly removed in Bedrock Preview/Beta 26.60.29. The 26.60 changelog also corrects invalid Armor Stand equipment swaps. Research has not verified another current direct Main Hand method for 26.60.

The button requires confirmed Survival mode, a valid inventory snapshot, a populated selected slot, and an empty hotbar destination. It passes the validated source ItemStack directly to the native FillingContainer::setItem method (which accepts a const ItemStack reference), then checks the local inventory again. If the method symbol is unavailable, it uses the existing vtable-slot fallback only after validating that the entry belongs to libminecraftpe.so. The status local_copy_applied means only that the second slot appeared on the client. It does not mean the server accepted or saved the change.

Test only in a copied world with a disposable item. Wait for any server correction, save and quit normally, reload the copied world, and verify both stacks again. A transient local copy is not a successful result.