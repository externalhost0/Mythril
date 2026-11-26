struct ReloadPush {
    Ptr<uint> out;
    uint value;
    uint pad;
};

[vk::push_constant]
ReloadPush push;

[vk::constant_id(3)]
const uint64_t kAdd = 0;

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main() {
    push.out[0] = push.value + uint(kAdd);
}
