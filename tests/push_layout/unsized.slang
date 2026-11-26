struct Buf { uint count; float data[]; };
struct Push { Ptr<Buf> buf; };
[vk::push_constant] Push push;
[shader("compute")][numthreads(1,1,1)]
void cs_main() { push.buf.data[push.buf.count] = 1; }
