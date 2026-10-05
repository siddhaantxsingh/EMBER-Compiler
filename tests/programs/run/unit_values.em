fn nothing() {}

fn main() {
    nothing();
    let units = [(), ()];
    print(units);
    print(nothing() == ());
    print(str(()) + "!");
}
