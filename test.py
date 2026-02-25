import clang.cindex
import tempfile, os, copy, re, json

MAIN_STUB = "int main(int argc, char** argv) { return 0; }"

def find_api(api : dict, apis_definition) -> bool:
    """ Find the api """
    api_hash = str(api)
    if api_hash == "{}":
        return True
    for a in apis_definition:
        a_hash = str(a)
        if api_hash == a_hash:
            return True
    return False


def get_stub_file(include_folder : list, public_headers : list):
    """ Return a temporary .cc file. """
    stub_file = tempfile.NamedTemporaryFile(suffix='.cc', delete=False).name
    public_header_lst = set()

    with open(public_headers, 'r') as ph:
        lines = ph.readlines()
        if len(lines) == 0:
            print(f"No header in public header file")
            exit(1)

        with open(stub_file, 'w') as tmp:
            for root, _, files in os.walk(include_folder):
                for h in files:
                    if h.endswith('.h') or h.endswith(".hh") or h.endswith('.hpp') and h in public_header_lst:
                        h_path = os.path.join(root, h)
                        print(h_path)
                        tmp.write(f"#include \"{h_path}\"\n")
            tmp.write("\n")
            tmp.write(MAIN_STUB)
    return stub_file

class ClangAPI:
    """ Class for retriving all enum, typedef and struct types and function declarations"""
    def __init__(self, include_folder : list,public_headers: string, namespace : list):
        self.function_declarations = list()
        self.apis_definition = list()
        self.all_types = set()
        self.type_incomplete = set()
        self.type_enums = set()
        self.include_paths = [f"-I{include_folder}",
                        "-I/usr/bin/../lib/gcc/x86_64-linux-gnu/9/../../../../include/c++/9",
                        "-I/usr/bin/../lib/gcc/x86_64-linux-gnu/9/../../../../include/c++/9/backward",
                        "-I/home/libfuzz/llvm-16/lib/clang/16/include",
                        "-I/usr/include/x86_64-linux-gnu",
                        "-I/usr/include"]

        clang.cindex.Config.set_library_file(os.path.join(os.path.expanduser('~'), "/home/mashmallow/.conda/envs/liberator/lib/python3.14/site-packages/clang/native/libclang.so"))
        index = clang.cindex.Index.create()
        tmp_file = get_stub_file(include_folder, public_headers)
        tu = index.parse(tmp_file, args=self.include_paths)
        root = tu.cursor

        self._traverse(root, include_folder, namespace)


    def get_types(self) -> set:
        """ Returns all type declarations."""
        return self.all_types

    def get_apis_definition(self) -> list:
        """ Returns all function declarations."""
        return self.apis_definition

    def get_incomplete_types(self) -> set:
        return self.type_incomplete

    def get_enum_types(self) -> set:
        """ Returns all enum types."""
        return self.type_enums

    def get_argument_info(self, type : clang.cindex.Cursor):
        info = {}

        atd = type.get_declaration()
        if atd.kind.is_declaration() and "::" not in type.spelling != "" and atd.underlying_typedef_type.spelling != "":
            type_str = atd.underlying_typedef_type.spelling

        else:
            type_str = type.spelling

        self.all_types.add(type_str)


        if "(*)" in type_str:
            info["type_clang"] = type_str
            info["const"] = []
        else:
            n_asterix = type_str.count("*") + type_str.count("[")
            if "[" in type_str:
                type_str = re.sub('\[\d*\]', '*', type_str)

            type_str_token = type_str.strip().replace("*", " * ").split()
            for bad_token in ["enum", "struct"]:
                if bad_token in type_str_token:
                    type_str_token.remove(bad_token)

            n_const = n_asterix
            const_pos = [False for _ in range(n_const+1)]

            i = 0
            for t in type_str_token:
                if t == "const":
                    if i < len(const_pos):
                        const_pos[i] = True

                    elif t == "*" and i == 1:
                        continue
                    else:
                        i = i+1

            while "const" in type_str_token:
                type_str_token.remove("const")

            info['type_clang'] = " ".join(type_str_token)
            info['const'] = const_pos

        return info

    def get_api(self, node : clang.cindex.Cursor, namespace : list) -> dict:
        """ Returns and api_obj with keys function_name, namespace and return_info"""
        api_obj = {}

        try:
            function_name = node.displayname[:node.displayname.index('(')]
        except ValueError:
            print(f"Can't find '(' in {node.displayname}")
            function_name = node.displayname
            return {}
        # function name
        api_obj["function_name"] = function_name
        # namespace the functions belongs to
        api_obj["namespace"] = copy.deepcopy(namespace)

        nt = node.type
        # extract return info
        rt = nt.get_result()
        api_obj["return_info"] = self.get_argument_info(rt)

        # extract arguments
        arguments_info = []
        for a in nt.argument_types():
            info = self.get_argument_info(a)
            arguments_info.append(copy.deepcopy(info))

        api_obj["arguments_info"] = arguments_info

        print(api_obj)

        return api_obj


    def _traverse(self, node : clang.cindex.Cursor, include_folder : list, namespace : list):
        if node.kind == clang.cindex.CursorKind.NAMESPACE:
            namespace += [node.displayname]

        for child in node.get_children():
            self._traverse(child, include_folder, copy.deepcopy(namespace))

        if node.type.kind == clang.cindex.TypeKind.FUNCTIONPROTO and include_folder in str(node.location.file):
            self.function_declarations.append(node)
            api = self.get_api(node, namespace)
            if not find_api(api, self.apis_definition):
                self.apis_definition.append(api)

        if node.kind in [clang.cindex.CursorKind.STRUCT_DECL, clang.cindex.CursorKind.TYPEDEF_DECL] and node.type.get_size() == -2:
            self.type_incomplete.add("%" + node.type.spelling)

        if node.kind == clang.cindex.CursorKind.ENUM_DECL:
            self.type_enums.add(node.type.spelling)

def _main():
    include_folder = "/home/mashmallow/source/liberator/targets/c-ares/work/include/"
    public_headers = "/home/mashmallow/source/liberator/targets/c-ares/public_headers.txt"

    clang_api = ClangAPI(include_folder, public_headers, [])

    target = os.environ["TARGET_NAME"]
    tmp_log = f"./alltypes_{target}.txt";
    exported_functions_file = f"./exported_functions.txt"
    incomplete_types = f"./incomplete_types.txt"
    apis_list = f"./api_list.txt"
    public_headers = f"./public_headers.txt"
    enum_list = f"./enum_list.txt"

    with open(tmp_log, 'w') as log:
        for t in clang_api.get_types():
            log.write(f"{t}\n")

    with open(exported_functions_file, 'w') as outf:
        for t in clang_api.get_apis_definition():
            outf.write(f"{t}\n")

    with open(incomplete_types, 'w') as outf:
        for t in clang_api.get_incomplete_types():
            outf.write(f"{t}\n")

    if enum_list is not None:
        with open(apis_list, 'w') as outf:
            for t in clang_api.get_enum_types():
                outf.write(f"{t}\n")

    with open(apis_list, 'w') as outf:
        for t in clang_api.get_api():
            outf.write(f"{json.dumps(t)}\n")

if __name__ == "__main__":
    _main()
