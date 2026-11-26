struct S { bool a; bool b; float c; };
struct X { double d; float f; };
struct Y { X x; float g; };
struct Push { Ptr<S> s; Ptr<Y> y; bool pa; bool pb; float pc; };
[vk::push_constant] Push push;
[shader("compute")][numthreads(1,1,1)]
void cs_main() {
    S v = push.s[0]; v.c = (v.a ? 1 : 0) + (v.b ? 2 : 0) + (push.pa ? 4 : 0) + (push.pb ? 8 : 0) + push.pc; push.s[0] = v;
    Y w = push.y[0]; w.g = w.x.f + float(w.x.d); push.y[0] = w;
}
