fn main() {
    fn early() {
        print(late());
    }
    early();
    let v = 1;
    fn late() -> Int {
        return v;
    }
}
