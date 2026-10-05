struct Empty {}
struct Node { value: Int, next: [Node] }
enum Maybe { Some(Int), None }

fn main() {
    print(());
    print(Empty {});
    print(Maybe::Some(3));
    print(Maybe::None);
    print([[1, 2], [3]]);
    print([1.0, 2.5]);
    print(["a", "b"]);
    print(Node { value: 1, next: [Node { value: 2, next: [] }] });
    var deep = [0];
    var i = 0;
    var nest: [[Int]] = [deep];
    print(nest);
    print(str(Maybe::Some(-1)) == "Maybe::Some(-1)");
}
