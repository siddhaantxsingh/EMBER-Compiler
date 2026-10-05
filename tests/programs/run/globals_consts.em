const LIMIT: Int = 10 * 10;
const NAME: String = "ember";
const HALF: Float = 1.0 / 2.0;
const ON: Bool = LIMIT > 50;

var counter = 0;
var greeting = "hi " + NAME;

fn bump() {
    counter += 1;
}

fn main() {
    bump();
    bump();
    print(counter);
    print(LIMIT);
    print(HALF);
    print(ON);
    print(greeting);
}
