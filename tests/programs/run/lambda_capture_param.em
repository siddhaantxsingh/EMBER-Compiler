fn make(n: Int) -> fn() -> Int {
    var local = n * 2;
    let f = fn() -> Int {
        local += n;
        return local;
    };
    return f;
}

fn main() {
    let f = make(5);
    print(f());
    print(f());
    let g = make(1);
    print(g());
    print(f());
}
