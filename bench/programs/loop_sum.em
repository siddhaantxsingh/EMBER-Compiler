// Category: tight integer loop with arithmetic and comparisons.
fn main() {
    var total = 0;
    var i = 0;
    while i < 20000000 {
        if i % 3 == 0 {
            total += i;
        } else {
            total -= 1;
        }
        i += 1;
    }
    print(total);
}
