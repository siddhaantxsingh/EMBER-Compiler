struct Node { value: Int, children: [Node] }

fn main() {
    let a = Node { value: 1, children: [] };
    push(a.children, a);
    let s = str(a);
    print(len(s) > 100);
    print(strings.substr(s, 0, 40));
}

import strings;
