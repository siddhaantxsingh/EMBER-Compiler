// args: --max-depth=500
fn down(n: Int) -> Int {
    return down(n + 1) + 1;
}

fn main() {
    print(down(0));
}
