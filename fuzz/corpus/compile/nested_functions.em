fn main() {
    fn is_even(n: Int) -> Bool {
        if n == 0 { return true; }
        return is_odd(n - 1);
    }
    fn is_odd(n: Int) -> Bool {
        if n == 0 { return false; }
        return is_even(n - 1);
    }
    print(is_even(10));
    print(is_odd(7));
    let base = 100;
    fn add_base(x: Int) -> Int { return x + base; }
    print(add_base(5));
}
