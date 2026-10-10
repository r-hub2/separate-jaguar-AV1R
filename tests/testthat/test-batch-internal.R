make_tree <- function() {
  root <- normalizePath(tempfile("tree"), mustWork = FALSE)
  dir.create(file.path(root, "a", "b"), recursive = TRUE)
  dir.create(file.path(root, "c"))
  root
}

test_that(".list_subdirs respects max_depth", {
  root <- make_tree()
  on.exit(unlink(root, recursive = TRUE))

  d1 <- AV1R:::.list_subdirs(root, max_depth = 1L)
  expect_setequal(basename(d1), c("a", "c"))

  d2 <- AV1R:::.list_subdirs(root, max_depth = 2L)
  expect_setequal(d2, file.path(root, c("a", "c", "a/b")))

  expect_length(AV1R:::.list_subdirs(root, max_depth = 0L), 0L)
})

test_that(".list_subdirs skips the excluded directory and its children", {
  root <- make_tree()
  on.exit(unlink(root, recursive = TRUE))

  d <- AV1R:::.list_subdirs(root, max_depth = 5L,
                            exclude = normalizePath(file.path(root, "a")))
  expect_identical(d, file.path(root, "c"))
})

test_that(".collect_tasks: single files, TIFF sequences and prefixes", {
  root <- normalizePath(tempfile("in"), mustWork = FALSE)
  dir.create(file.path(root, "seq"), recursive = TRUE)
  dir.create(file.path(root, "one"))
  on.exit(unlink(root, recursive = TRUE))
  file.create(file.path(root, c("x.avi", "y.mp4", "notes.txt")))
  file.create(file.path(root, "seq", c("f2.tif", "f1.tif")))
  file.create(file.path(root, "one", "z.tif"))

  ext <- c("avi", "mp4", "tif", "tiff")
  out <- file.path(root, "out")

  flat <- AV1R:::.collect_tasks(root, ext, recursive = FALSE,
                                max_depth = 5L, output_dir = out)
  expect_setequal(vapply(flat, `[[`, "", "out_name"),
                  c("x_av1.mp4", "y_av1.mp4"))

  tasks <- AV1R:::.collect_tasks(root, ext, recursive = TRUE,
                                 max_depth = 5L, output_dir = out)
  names(tasks) <- vapply(tasks, `[[`, "", "out_name")
  expect_setequal(names(tasks),
                  c("x_av1.mp4", "y_av1.mp4", "seq.mp4", "one_z_av1.mp4"))

  sq <- tasks[["seq.mp4"]]
  expect_true(sq$is_tiff_seq)
  expect_identical(sq$prefix, "seq")
  expect_identical(basename(sq$files), c("f1.tif", "f2.tif"))

  expect_false(tasks[["one_z_av1.mp4"]]$is_tiff_seq)
  expect_null(tasks[["x_av1.mp4"]]$prefix)
})

test_that(".collect_tasks ignores the output directory inside input", {
  root <- normalizePath(tempfile("in"), mustWork = FALSE)
  out  <- file.path(root, "out")
  dir.create(out, recursive = TRUE)
  on.exit(unlink(root, recursive = TRUE))
  file.create(file.path(out, "old.mp4"))

  tasks <- AV1R:::.collect_tasks(root, "mp4", recursive = TRUE,
                                 max_depth = 5L, output_dir = out)
  expect_length(tasks, 0L)
})
