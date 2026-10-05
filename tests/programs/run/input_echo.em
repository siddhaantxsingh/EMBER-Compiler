// stdin: alice\n42\n
fn main() {
    let name = input();
    let n = parse_int(input());
    print("hello " + name);
    print(n * 2);
    print(input() == "");
}
