kbase_conf_data = configuration_data()

kbase_conf_data.set('PAGE_SHIFT', cc.run('''
  #include <unistd.h>
  #include <stdio.h>
  #include <math.h>

  int main() {
      printf("%.0f", log2f(sysconf(_SC_PAGESIZE)));
      return 0;
  }''', dependencies : dep_m).stdout(),
  name : 'PAGE_SHIFT')

configure_file(output : 'panfrost-mali-base.h',
               input : 'panfrost-mali-base.h.meson.in',
               configuration : kbase_conf_data)
