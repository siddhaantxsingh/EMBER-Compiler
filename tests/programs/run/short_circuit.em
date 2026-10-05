var calls = 0;

fn noisy(v: Bool) -> Bool {
    calls += 1;
    return v;
}

fn main() {
    print(noisy(false) && noisy(true));
    print(calls);
    print(noisy(true) || noisy(false));
    print(calls);
    print(noisy(true) && noisy(false));
    print(calls);
}
