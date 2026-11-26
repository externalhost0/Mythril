struct ReloadPush {
    Ptr<uint> out;
    uint pad;
    uint value;
};

[vk::push_constant]
ReloadPush push;

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main() {
    push.out[0] = push.value + 1000;
}
